#pragma once

#include "Actions/actionTypes.h"
#include "Computer/taskContent.h"
#include "Goals/goalTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace revia::computer
{

// The bounded contract between the model that understands the request and the policy
// that takes the next step on the machine.
//
// Main is the only thing here that reads an open-ended sentence. What it produces is
// not an instruction and not an authorization: it is a description of one narrow piece
// of local progress, in a vocabulary small enough that the runtime can check every part
// of it before anything is allowed to act on it. "Open the conversation with the
// recipient I already identified" is a subgoal. "Message my sister" is not -- it names
// no target, names no evidence, and would hand a policy a mandate to finish a workflow
// nobody bounded.
//
// Everything in here is a *claim*. The runtime validates the claim against the task the
// user actually authorized, attaches the origin, scope and budget itself, and refuses
// combinations it does not support. No field here carries permission, approval or
// freshness, because a model asserting that something is approved is a model approving
// it.

// Versioned because it crosses a process boundary into stored records and, later, into
// training data. A record whose schema is not written down is a record nobody can
// safely read two versions from now.
//
// 1 -- the initial vocabulary: approved launch, focus, target resolution, one UI
//      interaction, exact-payload entry, waiting, and escalation.
inline constexpr std::uint32_t CurrentSubgoalSchema = 1;

// What kind of local progress is being asked for.
//
// Structured rather than free text so the runtime can decide whether it supports the
// combination at all. Extended only when a new value has a tested path behind it: an
// intent the runtime cannot check is worse than no intent, because it looks like a
// contract and is not one.
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

// How a target is described, as opposed to how it is identified.
//
// A description is matched against fresh candidates every time it is used. It is
// deliberately not an id: an automation id or a runtime id produced by a model is a
// claim about a live object that the model cannot have looked at, and treating one as
// an identity is how an action lands on whatever now occupies that slot.
//
// Empty fields are "unconstrained", not "any will do". A descriptor that matches more
// than one candidate is ambiguous and escalates; it never picks the first.
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

// A handle to user content the runtime holds and the model does not.
//
// The exact words of a message, a filename, a search string: the model decides *where*
// such a value belongs and never carries the value itself. It names a slot, and the
// runtime supplies the original after the action has been authorized.
//
// Two separate reasons, and both matter. A model that regenerates an exact message
// sends an approximation of what the user wrote, which is a wrong external effect
// wearing the right intent. And a payload that travelled through a model is a payload
// that can end up in a prompt, a log or a training set, which is a privacy boundary
// this type exists to keep intact.
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
    // Where the value came from. Travels with the reference because it changes what may
    // be done with the value: the user's own words are never regenerated, and a drafted
    // value is fixed the moment it is chosen so that the attempt that types it and the
    // check that reads it back are talking about the same sentence.
    //
    // Runtime-stamped like the rest of this type. A model naming a payload supplies an
    // id and nothing else; validation replaces the whole reference with the vault's own,
    // so a provenance claimed from outside is discarded rather than believed.
    ContentProvenance provenance = ContentProvenance::None;

    [[nodiscard]] bool Valid() const { return !id.empty(); }
};

// What finishing this subgoal would look like, as something checkable.
//
// Reuses the goal layer's typed postconditions rather than inventing a parallel
// vocabulary, so the evidence a subgoal asks for is evidence the runner already knows
// how to establish -- and so a subgoal cannot ask to be graded by a question the
// verification path does not support.
struct SubgoalPostcondition
{
    goals::PostconditionKind kind = goals::PostconditionKind::TextObserved;
    std::string subject;
    std::string value;
    // Set when the value is one the runtime holds rather than one the model wrote. The
    // check is then made against the original, for the same reason the action is.
    PayloadReference payload;
};

// Where a request came from, carried so the controller cannot relabel it.
//
// Idle work that presents itself as a user request would acquire the permissions,
// pacing and interruption rules that belong to something the user actually asked for.
// The runtime stamps this; nothing in a model's output can set it.
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

[[nodiscard]] SubgoalValidation ValidateSubgoal(
    const ComputerSubgoal& proposed,
    const SubgoalContext& context,
    const PayloadVault& vault);

// Proof that a subgoal went through validation, in a form nothing else can produce.
//
// This was a plain `bool validated` and that was not good enough. A bool is a claim
// anyone can make: a caller could set it, and every guarantee downstream -- scope
// checked, origin attached, payload confirmed -- would rest on a field the thing being
// checked could have written. That is precisely the shape of "no model-supplied
// approval is trusted" failing in the one place it matters.
//
// So the stamp is a type whose only mutator is the validator itself. It copies with the
// subgoal, because a validated subgoal genuinely stays validated as the runtime passes
// it around; it cannot be minted, because there is no way to reach the member from
// outside.
class SubgoalAuthority
{
public:
    SubgoalAuthority() = default;

    [[nodiscard]] bool Granted() const { return granted; }

private:
    friend SubgoalValidation ValidateSubgoal(
        const ComputerSubgoal& proposed,
        const SubgoalContext& context,
        const PayloadVault& vault);

    bool granted = false;
};

// One validated unit of local progress.
//
// Split deliberately into what a model proposed and what the runtime attached. The
// second half is not merely trusted-by-convention: `Validated()` is false until the
// runtime has stamped it, and the controller refuses to act on a subgoal that fails it.
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
    // The subgoal would submit, send or publish while the content this task exists to
    // place has not been placed.
    //
    // Not a permission check -- the consequence gate still refuses an external message
    // on its own terms, and it would refuse this one too. This is an *ordering* check,
    // and the two are different: the consequence gate asks whether sending is allowed,
    // and this asks whether there is anything to send yet. A task asked to put words in
    // a box has not been asked to press Send, and pressing it on an empty box is not a
    // smaller mistake for having been permitted.
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
