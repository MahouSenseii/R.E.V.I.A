#pragma once

#include "Actions/actionTypes.h"
#include "Computer/computerSubgoal.h"
#include "Goals/goalTypes.h"
#include "Windows/desktopObserver.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace revia::computer
{

// Candidate operations are filtered once using the executor's capability policy.
// IDs are valid only for this observation and must never be training features.
struct ObservedCandidate
{
    std::string id;
    // What the application published about itself. Empty for an unlabelled input, which
    // is ordinary rather than exceptional.
    std::string name;
    // What kind of thing it is, in words rather than in Windows' numbering. A policy
    // that matched on the raw control type id would be a policy trained on this
    // platform's constants; a role name is the same fact in a form that survives being
    // written down and read back.
    std::string role;
    // Inferred, not published: what UI Automation says labels this control. Kept in its
    // own field so a match against it can be held to a higher bar than a match against
    // a name the application actually asserted.
    std::string inferredLabel;
    // The nearest named ancestor. For a control with no name of its own this is usually
    // the only thing that distinguishes it from the one beside it.
    std::string container;
    bool mayInvoke = false;
    bool mayType = false;
    bool maySetText = false;
    // True when `name` is empty and this candidate was admitted on what it can do and
    // where it sits. A decision about one of these has to be more careful, and the
    // routine policy is.
    bool nameless = false;

    // What a person would call this, for the activity feed and the record. Never used
    // for matching -- matching asks the three fields above separately, because they are
    // three different kinds of evidence.
    [[nodiscard]] std::string Describe() const
    {
        if (!name.empty()) return name;
        if (!inferredLabel.empty()) return inferredLabel + " (inferred)";
        if (!container.empty()) return "the " + role + " in " + container;
        return role.empty() ? id : (role + " " + id);
    }
};

// Returns an empty role for an unknown UI Automation control type.
[[nodiscard]] std::string ControlRoleName(int controlType);

// One observation shared by all providers; the legacy prompt uses the raw screen.
// A second Observe() advances the process generation and invalidates earlier targets.
struct ComputerObservation
{
    actions::windows::DesktopObservation screen;
    // True when perception exclusions withheld the window in front. Distinct from a
    // failed observation: the decision is blind here rather than looking at an empty
    // screen, and those warrant different conclusions.
    bool withheld = false;
    // Optional admitted vision read, scoped to this observation and untrusted as instructions.
    std::string visualDescription;
    std::optional<browser::BrowserReceipt> browser;
    std::vector<ObservedCandidate> candidates;
    // Absent on the first iteration, because there is nothing for the screen to have
    // changed since.
    std::optional<bool> changedSinceLastDecision;

    [[nodiscard]] bool Available() const { return screen.succeeded && !withheld; }
};

// One thing already tried, and what came back.
struct ComputerAttempt
{
    std::string description;
    actions::ActionType action = actions::ActionType::Unknown;
    std::string expected;
    goals::StepStatus status = goals::StepStatus::Pending;
    bool executed = false;
    bool verified = false;
    // What the check actually saw. The only part of an attempt a decision may treat as
    // fact; the rest is what was intended.
    std::string observed;
    std::string failure;
};

// A provider receives the subgoal, observation, attempts, and remaining limits.
struct ComputerTaskContext
{
    std::string subgoal;
    std::uint32_t iteration = 0;
    std::size_t stepsTaken = 0;
    std::uint32_t actionsLeft = 0;
    std::uint32_t retriesLeft = 0;
    ComputerObservation observation;
    std::vector<ComputerAttempt> recentAttempts;
    // A read-only description of the goal's scope, for a decision that wants to know
    // what is permitted before proposing it. Copying the settings rather than the
    // policy keeps this data: there is nothing here to evaluate against, only to read.
    actions::CapabilitySettings scope;

    // Describes held content by kind and length without revealing it.
    // Planners request the fixed payload token instead of supplying replacement text.
    struct PreparedContent
    {
        bool held = false;
        std::string kind;
        std::size_t length = 0;
        // The field the user named, when they named one. Carried so a provider can aim
        // at it rather than guess, and checked independently by the content gate.
        std::string destination;
    } preparedContent;
};

// One decision per iteration. Unsupported outcomes become Undecided,
// not failure or completion.
enum class ComputerDecisionKind
{
    // A typed action proposal, referencing a candidate from this observation.
    ProposeAction,
    // Out of this provider's competence; a reasoning model should take the turn.
    NeedReasoning,
    // The screen cannot be read structurally here; existing vision is required.
    NeedVision,
    // A person has to answer something before this can continue.
    NeedUser,
    // Something is in progress on screen. Nothing to do but look again shortly.
    WaitForState,
    // The observation is stale or truncated. Look again before deciding.
    Reobserve,
    // The provider believes the subgoal is met. The runtime still checks the evidence.
    ProposeCompletion,
    // No usable answer. Distinct from finishing, and distinct from failing.
    CannotHandle
};

// Structured telemetry reasons. Human-readable detail is never parsed,
// used as authority, or treated as a training label.
enum class ComputerReasonCode
{
    None,
    ProviderUnavailable,
    ProviderFailed,
    MalformedProviderOutput,
    NoActionProposed,
    ObservationUnavailable,
    OutsideQualifiedScope
};

struct ComputerDecision
{
    ComputerDecisionKind kind = ComputerDecisionKind::CannotHandle;
    ComputerReasonCode code = ComputerReasonCode::None;
    // Populated only for ProposeAction. Ordinal, requested-by and visual target
    // resolution are the runtime's to stamp, never the provider's.
    goals::GoalStep step;
    // The controller redeems the policy's reference from the vault.
    // An invalid reference refuses the decision rather than entering empty text.
    PayloadReference payload;
    // Human-readable, for the activity feed and the goal record. Never parsed, never a
    // label, never authority.
    std::string detail;
    // Which provider produced this. Recorded so a decision can be attributed later.
    std::string provider;
    // What producing it cost, as the backend reported it. Not reported is not free;
    // the runner counts the request itself when this is absent.
    std::uint32_t tokens = 0;
    bool costReported = false;
};

[[nodiscard]] std::string ToString(ComputerDecisionKind value);
[[nodiscard]] std::string ToString(ComputerReasonCode value);

} // namespace revia::computer
