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

// One candidate on screen, as a decision sees it.
//
// A typed view over an ObservedControl, already filtered to the operations the goal's
// own scope would not refuse outright. It exists so that filtering is done once, from
// the same CapabilityPolicy the executor uses, rather than re-implemented per provider
// -- and so a provider that is not a language model has something to rank.
//
// `id` is a selection reference valid for one observation and nothing else. It is not
// a feature, it is not stable across looks, and it must never be trained on: a policy
// that learned an automation id learned this machine rather than the task.
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

// The role name for a UI Automation control type id.
//
// Lives here rather than in Windows because it is the vocabulary decisions are written
// in, and because a test needs to be able to build a candidate without a desktop. An id
// this table does not know becomes an empty role, which a descriptor can then only
// match by leaving the role unconstrained -- not by accident.
[[nodiscard]] std::string ControlRoleName(int controlType);

// The one look at the machine taken for an iteration, shared by every provider.
//
// It carries the raw DesktopObservation as well as the typed candidates, because the
// legacy prompt is built from DesktopObservation::Describe and must keep being built
// from it. Re-deriving that text from the typed view would be a behaviour change
// wearing a refactor's clothes.
//
// Taking a second look is what this type exists to prevent. `observationGeneration` is
// a process-wide counter bumped by every Observe(), claimed before the look can fail,
// and CompareVisualTarget refuses a target that is not the newest -- so a provider
// observing for itself would invalidate a target another provider had already chosen
// correctly, and the executor would refuse a click that was right when it was decided.
struct ComputerObservation
{
    actions::windows::DesktopObservation screen;
    // True when perception exclusions withheld the window in front. Distinct from a
    // failed observation: the decision is blind here rather than looking at an empty
    // screen, and those warrant different conclusions.
    bool withheld = false;
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

// Everything a decision provider is given, and nothing it is not.
//
// Deliberately not the Goal. A provider has no business holding the goal's capability
// scope object, its store id or its budget struct; it gets the subgoal, what is on
// screen, what has been tried, and what is left.
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

    // What the runtime is holding for this task, described and never revealed.
    //
    // A provider is told that content of this kind and this length exists so that it
    // can choose a destination for it. It is not told the content, and the grammar the
    // legacy path is asked under constrains the value field to a fixed token, so the
    // only thing any planner can put where the words go is a request for them.
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

// What a provider decided. One of these per iteration, never two.
//
// The four the legacy provider can produce are ProposeAction, ProposeCompletion,
// CannotHandle and NeedReasoning-as-itself. The rest are the vocabulary the bounded
// providers in later stages need, and the runtime already maps them honestly: anything
// it cannot act on becomes "no usable answer", which is a real outcome and not a
// failure.
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

// Structured, so telemetry can be counted rather than read.
//
// Free-form explanation is deliberately absent. It would be a second unreviewed
// channel out of a model, it cannot be aggregated, and it must never become a training
// label. `detail` below carries text for the activity feed and the record; nothing
// parses it.
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
    // Which runtime-held value belongs in this step's action, when one does.
    //
    // The policy names the slot and the controller fills it from the vault. That
    // ordering is the whole point of a payload reference: a decision carrying the user's
    // exact words is a decision that has been given them, and the design is that it
    // never is. An unredeemable reference refuses the decision rather than executing it
    // with an empty value.
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
