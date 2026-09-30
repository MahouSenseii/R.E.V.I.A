#pragma once

#include "Actions/actionTypes.h"
#include "Computer/taskContent.h"
#include "Goals/goalTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace revia::computer
{

// A model proposes bounded local progress; the runtime validates it against
// the authorized task and attaches origin, scope, budget, and authority.

// Persisted subgoal schema: launch, focus, resolution, interaction,
// exact-payload entry, waiting, and escalation.
inline constexpr std::uint32_t CurrentSubgoalSchema = 1;

// Add intents only when the runtime has a tested validation and execution path.
enum class SubgoalIntent
{
    // No usable intent was produced, or validation stripped one it does not support.
    // Always falls back rather than guessing.
    Unspecified,
    // Start an application the scope already approves. Permitted without an observation
    // because a launch resolves from the approved application list rather than from
    // what is currently on screen.
    LaunchApplication,
    // Bring an already-running window to the front.
    FocusWindow,
    // Find and report a target without touching it. Read-only, and the honest answer to
    // "which of these is the one?" when the answer should be shown rather than acted on.
    ResolveTarget,
    // One interaction with one control: press it, toggle it, choose it.
    InteractWithControl,
    // Put an exact, runtime-held payload into a named field. The payload itself is
    // never in the subgoal; see PayloadReference.
    EnterPayload,
    // Something is in progress. Wait and look again rather than acting into it.
    WaitForState,
    // This needs a person or a reasoning model. Named explicitly so that "I should stop"
    // is a first-class answer and not an absence of one.
    Escalate
};

[[nodiscard]] std::string ToString(SubgoalIntent value);
[[nodiscard]] SubgoalIntent SubgoalIntentFromString(const std::string& value);

// Match descriptions against fresh candidates, never model-supplied identities.
// Empty fields are unconstrained; multiple matches escalate rather than choosing one.
struct TargetDescriptor
{
    // The window the target must be in. Checked against the scope's approved
    // applications before it is checked against the screen.
    std::string application;
    // A fragment of the window title, when the application alone is not enough.
    std::string windowTitle;
    // The accessible name, which is the thing a person would read off the screen.
    std::string name;
    // The control role, in the observer's vocabulary: button, edit, list item.
    std::string role;
    // The container the target sits in, when that is what distinguishes it -- the
    // "Send" in the compose pane rather than the one in the toolbar.
    std::string container;

    [[nodiscard]] bool Empty() const
    {
        return application.empty() && windowTitle.empty() && name.empty() &&
            role.empty() && container.empty();
    }
};

// References exact content held by the runtime. The model selects a destination;
// the runtime supplies the original only after authorization.
struct PayloadReference
{
    // Opaque and runtime-minted. Never a substring of the value, never guessable, and
    // never meaningful to the model beyond "the thing to put here".
    std::string id;
    // What kind of value it is, so a policy can tell a message from a filename without
    // being shown either.
    std::string kind;
    // How long it is, which is enough to refuse an oversized entry without reading it.
    std::size_t length = 0;
    // Runtime-stamped provenance: originals are preserved and selected drafts stay fixed.
    // Validation replaces claimed references with the vault's authoritative copy.
    ContentProvenance provenance = ContentProvenance::None;

    [[nodiscard]] bool Valid() const { return !id.empty(); }
};

// Uses the goal runner's supported, typed postconditions.
struct SubgoalPostcondition
{
    goals::PostconditionKind kind = goals::PostconditionKind::TextObserved;
    std::string subject;
    std::string value;
    // Set when the value is one the runtime holds rather than one the model wrote. The
    // check is then made against the original, for the same reason the action is.
    PayloadReference payload;
};

// Runtime-stamped origin controls permissions, pacing, and interruption.
// Model output cannot set or relabel it.
enum class RequestOrigin
{
    // No established origin. Treated as the most restricted thing it could be.
    Unknown,
    // The user asked for this, in this session, in so many words.
    UserDirected,
    // She decided to do it. Narrower permissions, and it yields to the user.
    Autonomous
};

[[nodiscard]] std::string ToString(RequestOrigin value);

struct ComputerSubgoal;
struct SubgoalContext;
struct SubgoalValidation;
class PayloadVault;

[[nodiscard]] SubgoalValidation ValidateSubgoal(const ComputerSubgoal& proposed, const SubgoalContext& context, const PayloadVault& vault);

// Only ValidateSubgoal can mint this stamp; it copies with validated subgoals.
class SubgoalAuthority
{
public:
    SubgoalAuthority() = default;

    [[nodiscard]] bool Granted() const { return granted; }

private:
    friend SubgoalValidation ValidateSubgoal(const ComputerSubgoal& proposed, const SubgoalContext& context, const PayloadVault& vault);

    bool granted = false;
};

// Separates model proposals from runtime authority. The controller refuses
// subgoals without a validation stamp.
struct ComputerSubgoal
{
    std::uint32_t schemaVersion = CurrentSubgoalSchema;
    // Identifies this subgoal in records and telemetry. Runtime-minted.
    std::string id;
    // Which goal this is a piece of, so a decision can be joined back to the run.
    std::string goalId;

    // ---- proposed, and therefore checked ----

    SubgoalIntent intent = SubgoalIntent::Unspecified;
    // For people, not for parsing. Shown in the activity feed and stored in the record;
    // nothing branches on it, and it is never a second contradictory requirement beside
    // the typed postconditions.
    std::string description;
    TargetDescriptor target;
    // Exactly one payload per subgoal. More than one would mean a step that fills a
    // form, and a step that fills a form is a workflow rather than a bounded move.
    PayloadReference payload;
    std::vector<SubgoalPostcondition> postconditions;

    // ---- attached by the runtime, never by a model ----

    RequestOrigin origin = RequestOrigin::Unknown;
    // The goal's scope as narrowed for this run. Read-only here; a policy may consult
    // it to avoid proposing what would be refused, and cannot alter it.
    actions::CapabilitySettings scope;
    std::uint32_t actionsLeft = 0;
    std::uint32_t retriesLeft = 0;
    // Granted once the runtime has validated the proposal and attached the above. A
    // subgoal that has not been through validation is a model's opinion, and this is
    // the only thing that can tell the difference.
    SubgoalAuthority authority;

    [[nodiscard]] bool Validated() const
    {
        return authority.Granted() && schemaVersion == CurrentSubgoalSchema &&
            intent != SubgoalIntent::Unspecified;
    }
};

// Why a proposed subgoal was refused. Structured so refusals can be counted rather
// than read, and so the user-visible explanation can be precise about which boundary
// was reached.
enum class SubgoalRejection
{
    None,
    // The proposal named a schema this build does not implement. Refused rather than
    // best-effort parsed: a partially understood contract is not a contract.
    UnsupportedSchema,
    // No intent, or one this build has no tested path for.
    UnsupportedIntent,
    // The intent needs a target and none was described, or needs a payload and none
    // was referenced.
    IncompleteForIntent,
    // The target names an application the goal's scope does not approve. This is the
    // boundary a subgoal most plausibly tries to step over, and it is checked against
    // the scope rather than against what happens to be on screen.
    OutsideScope,
    // The payload reference names nothing the runtime is holding. A model naming a
    // payload it invented gets nothing, rather than an empty string.
    UnknownPayload,
    // A postcondition asked for evidence in a form the verification path cannot
    // establish, or asked about something other than the subgoal's own target.
    UnverifiablePostcondition,
    // A field was longer than the contract allows.
    OversizedField,
    // Submission was proposed before the required content was placed.
    // This ordering check is separate from permission to send.
    SendBeforePlacement
};

[[nodiscard]] std::string ToString(SubgoalRejection value);

// Runtime-minted, like every other identity here. A subgoal id a model chose would be
// a model naming a row in the runtime's own record.
[[nodiscard]] std::string NewSubgoalId();

struct SubgoalValidation
{
    bool accepted = false;
    SubgoalRejection rejection = SubgoalRejection::None;
    // For the activity feed. Never parsed.
    std::string detail;
    // Populated only when accepted. Carries the runtime's own stamps.
    ComputerSubgoal subgoal;
};

} // namespace revia::computer
