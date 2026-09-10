#pragma once

#include "Actions/actionTypes.h"

#include <cstdint>
#include <string>

namespace revia::policy
{

// One place that decides whether a desktop effect may happen.
//
// It exists because the same consequence can be reached three ways -- a click, a
// keystroke, or a UI Automation pattern -- and before this, only the first was checked.
// Sending a message by clicking Send was gated; sending it by invoking the same button
// through UIA was not. Parallel executors with different rules is how a boundary becomes
// decorative, so the rules live here and the executors ask.
//
// What this enforces:
//   * an effect needs the same authority however it is reached;
//   * missing or ambiguous evidence about a target is not treated as permission;
//   * effects are a set, so an action that both edits and sends needs both.
//
// What it does not enforce, and cannot:
//   * it is not a sandbox. It runs inside the same process it is protecting, and a
//     defect above it is not caught by it;
//   * label reading is heuristic and English-shaped. It recognizes what it recognizes;
//   * a check immediately before SendInput is still a check-to-use race. The desktop
//     is not transactional and nothing here makes it so.

// Effects are a set, not a ladder.
//
// A ladder forces "edits the document" and "sends it to someone" onto one axis and keeps
// only the larger. An action that does both needs the authority for both, so they are
// bits.
enum class DesktopEffect : std::uint32_t
{
    None              = 0u,
    UserContent       = 1u << 0,
    ExternalMessage   = 1u << 1,
    Financial         = 1u << 2,
    Destructive       = 1u << 3,
    AccountOrSecurity = 1u << 4,
    CommandSurface    = 1u << 5
};

using DesktopEffects = std::uint32_t;

[[nodiscard]] constexpr DesktopEffects operator|(const DesktopEffect a, const DesktopEffect b)
{
    return static_cast<DesktopEffects>(a) | static_cast<DesktopEffects>(b);
}
[[nodiscard]] constexpr DesktopEffects operator|(const DesktopEffects a, const DesktopEffect b)
{
    return a | static_cast<DesktopEffects>(b);
}
[[nodiscard]] constexpr bool HasEffect(const DesktopEffects set, const DesktopEffect one)
{
    return (set & static_cast<DesktopEffects>(one)) != 0u;
}

// How much is actually known about what the action will land on.
//
// The distinction that matters: Missing is not Routine. Before this existed, a control
// with no readable label produced no dangerous keyword and was therefore authorized,
// which meant the check passed most reliably exactly when it knew least.
enum class EvidenceQuality
{
    // Fresh, resolved, and carrying a label that means something.
    Verified,
    // Resolved, but the label does not say what it does. "OK" and "Confirm" name the
    // gesture rather than the consequence.
    Ambiguous,
    // Not resolved, unnamed, or observed too long ago to still be about this moment.
    Missing
};

// What the action will land on. Populated by whichever executor is asking; the fields
// are plain so this header stays free of Windows and stays unit-testable.
struct TargetEvidence
{
    bool resolved = false;
    std::string controlName;
    std::string automationId;
    // Surrounding context. A "Confirm" inside a window titled "Delete account" is not
    // the same button as a "Confirm" inside "Save preferences".
    std::string windowTitle;
    std::string executable;
    int controlType = 0;
    // From UI Automation, not from the label. The one signal here that is measured.
    bool isPassword = false;
    // The observation is older than the freshness bound, so it describes a machine that
    // may have moved on.
    bool stale = false;
};

// What is about to be done, independent of what it lands on.
enum class DesktopOperation
{
    Observe,
    PointerActivate,
    // A chord that could commit something.
    KeyActivate,
    // A chord known only to move around: arrows, tab, page up.
    KeyNavigate,
    TextEntry,
    // Literal text carrying an embedded newline, which presses Enter on the way past.
    TextEntryWithActivation,
    SetValue,
    Invoke,
    LaunchApplication
};

enum class AuthorizationVerdict
{
    Allow,
    // Not a refusal on the merits: it needs a specific human yes that this build cannot
    // yet obtain, so callers currently render it as a refusal with the reason attached.
    RequireApproval,
    Refuse
};

struct AuthorizationDecision
{
    AuthorizationVerdict verdict = AuthorizationVerdict::Refuse;
    DesktopEffects effects = 0u;
    EvidenceQuality evidence = EvidenceQuality::Missing;
    // Actionable and free of the content it was protecting: it names the control and the
    // effect, never the text that was going to be typed.
    std::string reason;
};

struct AuthorizationRequest
{
    DesktopOperation operation = DesktopOperation::Observe;
    TargetEvidence evidence;
    // Whether Revia raised this herself. Runtime-owned: it comes from the requestedBy
    // the session stamps at the entry points, and model JSON has no field that reaches
    // it. A user who asked for something specific has already supplied the judgement
    // that an unreadable label would otherwise have to stand in for.
    bool autonomousOrigin = false;
    // A verified disposable workspace. Unknown labels there are not a reason to stop,
    // because there is nothing in it that matters.
    bool insideApprovedScratch = false;
};

// The ceiling is stored as one ordered class for backward compatibility with capability
// files that already exist. This expands it into the set it stands for.
[[nodiscard]] DesktopEffects EffectsPermittedByCeiling(actions::ConsequenceClass ceiling);

[[nodiscard]] EvidenceQuality AssessEvidence(const AuthorizationRequest& request);
[[nodiscard]] DesktopEffects AssessEffects(const AuthorizationRequest& request);
// True for anything that changes state. Observation and pure navigation are not gated,
// so inspecting an unfamiliar window stays possible.
[[nodiscard]] bool IsCommittingOperation(DesktopOperation operation);
[[nodiscard]] std::string ToString(DesktopOperation operation);
[[nodiscard]] std::string ToString(EvidenceQuality quality);
[[nodiscard]] std::string ToString(AuthorizationVerdict verdict);
[[nodiscard]] std::string DescribeEffects(DesktopEffects effects);

[[nodiscard]] AuthorizationDecision AuthorizeDesktopEffect(
    const AuthorizationRequest& request,
    const actions::CapabilitySettings::DesktopControl& settings);

// What executors call.
//
// It exists so there is exactly one place that turns a typed action plus a target into
// yes or no. Both the synthesized-input executor and the UI Automation executor go
// through it, because the alternative -- each backend deciding for itself -- is how
// clicking Send came to be checked while invoking the same button through UIA was not.
//
// Task origin is taken from the request rather than passed in separately, so a caller
// cannot quietly claim a user asked for something.
// A short fingerprint of the settings an authorization was granted under.
//
// Bindings and approvals carry it so that changing a permission invalidates anything
// already decided under the old one. Without it, a grant made a moment before the owner
// tightened a switch would still be spendable afterwards.
[[nodiscard]] std::string PolicyVersion(
    const actions::CapabilitySettings::DesktopControl& settings);

[[nodiscard]] bool AuthorizeOrExplain(
    DesktopOperation operation,
    const TargetEvidence& evidence,
    const actions::CapabilitySettings::DesktopControl& settings,
    const actions::ActionRequest& request,
    std::string& outFailure);

} // namespace revia::policy
