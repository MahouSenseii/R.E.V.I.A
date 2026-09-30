#pragma once

#include "Actions/actionTypes.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <string>

namespace revia::policy
{

// Shared authorization for clicks, keystrokes, and UIA; all effects require authority
// and missing/ambiguous target evidence grants none. This is not a sandbox: labels are
// heuristic and a pre-injection check cannot eliminate desktop races.

// Effect bits combine: an action that edits and sends requires authority for both.
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

// Missing evidence is not Routine and must not become permission.
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

[[nodiscard]] AuthorizationDecision AuthorizeDesktopEffect(const AuthorizationRequest& request,
    const actions::CapabilitySettings::DesktopControl& settings);

// Settings fingerprint carried by bindings and approvals; permission changes
// invalidate decisions made under the previous policy.
[[nodiscard]] std::string PolicyVersion(const actions::CapabilitySettings::DesktopControl& settings);

// What a person is being asked to approve. It names the control and what that control
// would do, and deliberately never carries the message body: approving "Send" is
// approving an effect on a control, not endorsing text the prompt could be used to
// display. Bounded and free of content, exactly like AuthorizationDecision::reason.
struct ApprovalPrompt
{
    std::string controlName;
    std::string application;
    std::string windowTitle;
    // The authorization's own sentence about why this stopped.
    std::string reason;
};

// Runtime-owned approval gate survives executor reloads via shared_ptr.
// Model output cannot reach it; unanswered approval is refusal.
class DesktopApprovalGate
{
public:
    class TaskApproval
    {
    public:
        TaskApproval(const TaskApproval&) = delete;
        TaskApproval& operator=(const TaskApproval&) = delete;
        ~TaskApproval();
    private:
        friend class DesktopApprovalGate;
        TaskApproval(DesktopApprovalGate& owner, std::string goalId, bool messaging);
        DesktopApprovalGate& owner;
        std::string goalId;
    };

    using Handler = std::function<bool(const ApprovalPrompt&)>;

    // Runtime-only, bounded to a submitted goal and removed on every exit.
    [[nodiscard]] TaskApproval ApproveTask(std::string goalId, bool messaging);
    [[nodiscard]] std::optional<bool> TaskDecision(const std::string& requestedBy, DesktopEffects effects) const;

    void SetHandler(Handler handler);
    // False when no handler is installed, which is the correct answer for a headless
    // run: nobody is there, so nobody approved it.
    [[nodiscard]] bool Ask(const ApprovalPrompt& prompt) const;
    [[nodiscard]] bool HasHandler() const;

private:
    mutable std::mutex mutex;
    Handler handler;
    std::map<std::string, DesktopEffects> taskApprovals;
};

// `gate` is optional. Without one the behavior is exactly what it was before approvals
// existed: RequireApproval renders as a refusal.
[[nodiscard]] bool AuthorizeOrExplain(DesktopOperation operation, const TargetEvidence& evidence,
    const actions::CapabilitySettings::DesktopControl& settings, const actions::ActionRequest& request, std::string& outFailure,
    const DesktopApprovalGate* gate = nullptr);

} // namespace revia::policy
