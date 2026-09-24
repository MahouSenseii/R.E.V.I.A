#include "Computer/subgoalValidator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace revia::computer
{

namespace
{

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// Control characters in a descriptor are not a descriptor. They are what output looks
// like when it has stopped being the thing it claims to be, and a name carrying a
// newline would also be a name that formats oddly everywhere it is later shown.
bool Printable(const std::string& value)
{
    return std::none_of(value.begin(), value.end(), [](const unsigned char character)
    {
        return character < 0x20 && character != 0x09;
    });
}

bool FieldAcceptable(const std::string& value, const std::size_t limit)
{
    return value.size() <= limit && Printable(value);
}

SubgoalValidation Refuse(const SubgoalRejection rejection, std::string detail)
{
    SubgoalValidation validation;
    validation.rejection = rejection;
    validation.detail = std::move(detail);
    return validation;
}

// Whether a postcondition asks about the subgoal's own target.
//
// The guard against a convenient predicate. A subgoal may not ask to be graded on
// something adjacent to what it was asked to do -- "the window is open" is not evidence
// that the conversation with the named recipient is the one open -- so a condition has
// to be about the target that was described, or about the payload that was referenced.
bool PostconditionIsRelevant(
    const SubgoalPostcondition& condition,
    const ComputerSubgoal& subgoal)
{
    using goals::PostconditionKind;
    const std::string subject = Lowered(condition.subject);
    const std::string value = Lowered(condition.value);
    const std::string targetName = Lowered(subgoal.target.name);
    const std::string targetApplication = Lowered(subgoal.target.application);

    switch (condition.kind)
    {
        case PostconditionKind::ForegroundApplicationIs:
            // The application it names has to be the application the subgoal is about.
            return !targetApplication.empty() && value == targetApplication;

        case PostconditionKind::ControlValueIs:
        {
            // The control it names has to be the control the subgoal is about, and the
            // value has to be the payload rather than something the model chose.
            //
            // A subgoal whose target has no name identifies it by container, so that is
            // what a condition about it may name. Requiring a name here would make an
            // unnamed target ungradeable and therefore unusable.
            const std::string targetContainer = Lowered(subgoal.target.container);
            const bool aboutThisTarget = targetName.empty()
                ? (!targetContainer.empty() && subject == targetContainer)
                : subject == targetName;
            if (!aboutThisTarget) return false;
            return condition.payload.Valid() || subgoal.payload.Valid();
        }

        case PostconditionKind::DirectoryHasEntry:
        case PostconditionKind::DirectoryLacksEntry:
        case PostconditionKind::FileContains:
            // Filesystem evidence for a subgoal whose vocabulary is windows and
            // controls. The runtime has no way to tie the two together here, so it does
            // not pretend to: these belong to the planned filesystem path, not to a
            // desktop subgoal.
            return false;

        case PostconditionKind::TextObserved:
        default:
            // Untyped, and therefore not something a subgoal may be graded by. It is
            // retained on a *step* for compatibility; a subgoal asking for it is asking
            // for the weak rule by name.
            return false;
    }
}

} // namespace

bool ScopeApprovesApplication(
    const actions::CapabilitySettings& scope, const std::string& application)
{
    if (application.empty()) return false;
    // Matched the same way CapabilityPolicy matches it, by the same rule, because two
    // spellings of "is this allowed" is how they come to disagree -- and the one that
    // disagrees permissively is the one that matters.
    const std::string wanted = Lowered(application);
    return std::any_of(scope.approvedApplications.begin(), scope.approvedApplications.end(),
        [&wanted](const std::string& allowed) { return Lowered(allowed) == wanted; });
}

bool NamesASubmission(const std::string& control)
{
    static const std::array<std::string_view, 8> words{
        "send", "submit", "post", "publish", "share", "reply all", "reply", "confirm"};
    const std::string lowered = Lowered(control);
    return std::any_of(words.begin(), words.end(), [&](const std::string_view word) {
        return lowered.find(word) != std::string::npos;
    });
}

SubgoalValidation ValidateSubgoal(
    const ComputerSubgoal& proposed,
    const SubgoalContext& context,
    const PayloadVault& vault)
{
    if (proposed.schemaVersion != CurrentSubgoalSchema)
    {
        return Refuse(SubgoalRejection::UnsupportedSchema,
            "The proposed subgoal uses a contract this build does not implement.");
    }
    if (proposed.intent == SubgoalIntent::Unspecified)
    {
        return Refuse(SubgoalRejection::UnsupportedIntent,
            "The proposal named no intent this build can act on.");
    }

    // Size and shape first, before anything is read closely. A field long enough to be
    // a paragraph has stopped being a descriptor, and refusing it early means nothing
    // downstream has to defend against it.
    if (!FieldAcceptable(proposed.description, MaximumSubgoalDescription) ||
        !FieldAcceptable(proposed.target.application, MaximumDescriptorField) ||
        !FieldAcceptable(proposed.target.windowTitle, MaximumDescriptorField) ||
        !FieldAcceptable(proposed.target.name, MaximumDescriptorField) ||
        !FieldAcceptable(proposed.target.role, MaximumDescriptorField) ||
        !FieldAcceptable(proposed.target.container, MaximumDescriptorField))
    {
        return Refuse(SubgoalRejection::OversizedField,
            "A field in the proposed subgoal was longer than the contract allows, or "
            "carried characters a descriptor cannot contain.");
    }
    if (proposed.postconditions.size() > MaximumSubgoalPostconditions)
    {
        return Refuse(SubgoalRejection::OversizedField,
            "The proposal carried more completion conditions than a single bounded "
            "subgoal can have.");
    }

    // What each intent actually needs to be actionable. An intent whose requirements
    // are not met is refused rather than attempted with the missing half guessed.
    const bool needsTarget =
        proposed.intent == SubgoalIntent::LaunchApplication ||
        proposed.intent == SubgoalIntent::FocusWindow ||
        proposed.intent == SubgoalIntent::ResolveTarget ||
        proposed.intent == SubgoalIntent::InteractWithControl ||
        proposed.intent == SubgoalIntent::EnterPayload;
    const bool needsControlName =
        proposed.intent == SubgoalIntent::InteractWithControl ||
        proposed.intent == SubgoalIntent::EnterPayload;
    const bool needsPayload = proposed.intent == SubgoalIntent::EnterPayload;

    // Three things a task can do to a field, and they are not the same thing.
    //
    // Resolving finds it. Entering puts content in it. Submitting sends what is in it
    // somewhere that cannot be taken back. A task asked to do the second has not been
    // asked to do the third, and the live run showed a small model proposing exactly
    // that -- "Send" -- on a task whose content had not been entered yet.
    //
    // Refused here rather than left to the consequence gate, because the two are asking
    // different questions. The consequence gate asks whether sending is permitted at
    // all; this asks whether there is anything to send. An empty message the user never
    // authorized is not made acceptable by the fact that sending was approved.
    if (context.contentPending &&
        proposed.intent == SubgoalIntent::InteractWithControl &&
        NamesASubmission(proposed.target.name))
    {
        return Refuse(SubgoalRejection::SendBeforePlacement,
            "This task is to put content in a field, and that has not happened yet. "
            "Pressing " + proposed.target.name + " now would submit whatever is there.");
    }

    if (needsTarget && proposed.target.Empty())
    {
        return Refuse(SubgoalRejection::IncompleteForIntent,
            "The proposed subgoal named no target to act on.");
    }
    if (needsTarget && proposed.target.application.empty())
    {
        // Every desktop action names its executable; a subgoal that does not is a
        // subgoal whose scope cannot be checked before it is acted on.
        return Refuse(SubgoalRejection::IncompleteForIntent,
            "The proposed subgoal named no application, so its scope cannot be checked.");
    }
    if (needsControlName)
    {
        // A target has to be identifiable, which is not the same as having a name.
        //
        // This used to require a name outright, and that was right while the observer
        // dropped every unnamed control: a subgoal naming nothing could match nothing.
        // Now that a nameless field can be identified by the panel it sits in
        // (ISSUE-REVIA-0070), refusing here would make the observer fix unreachable
        // from the one path that produces subgoals.
        //
        // Container *and* role together, because neither alone identifies anything: a
        // panel holds several controls, and a role matches every control of that kind
        // in the window. The routine policy still escalates when the pair turns out to
        // match more than one thing on the actual screen -- this only decides whether
        // the subgoal is well-formed enough to be worth looking.
        const bool namedTarget = !proposed.target.name.empty();
        const bool locatedTarget =
            !proposed.target.container.empty() && !proposed.target.role.empty();
        if (!namedTarget && !locatedTarget)
        {
            return Refuse(SubgoalRejection::IncompleteForIntent,
                "The proposed subgoal asked to touch a control without naming one or "
                "saying where it sits.");
        }
    }

    // Scope, checked against what the user authorized and not against what is on
    // screen. A window that appeared during the run is not permission to use it.
    if (needsTarget && !ScopeApprovesApplication(context.scope, proposed.target.application))
    {
        return Refuse(SubgoalRejection::OutsideScope,
            "The proposed subgoal named an application this task is not approved to "
            "touch.");
    }
    if (proposed.intent == SubgoalIntent::LaunchApplication &&
        !context.scope.desktopControl.applicationLaunch)
    {
        return Refuse(SubgoalRejection::OutsideScope,
            "Starting an application is not enabled for this task.");
    }

    // Payloads. A reference the vault does not hold gets nothing -- not an empty
    // string, which in a field about to be submitted is an empty message actually sent.
    //
    // A proposal names a payload by id and by nothing else: it has never seen the value
    // and cannot say how long it is. So the reference is replaced with the vault's own
    // description before anything is checked against it -- a length check performed
    // against a number the proposal supplied would not be a check.
    std::optional<PayloadReference> heldPayload;
    if (proposed.payload.Valid())
    {
        heldPayload = vault.Describe(proposed.payload);
        if (!heldPayload.has_value())
        {
            return Refuse(SubgoalRejection::UnknownPayload,
                "The proposed subgoal referenced content the runtime is not holding.");
        }
    }

    if (needsPayload)
    {
        if (!heldPayload.has_value())
        {
            return Refuse(SubgoalRejection::IncompleteForIntent,
                "The proposed subgoal asked to enter a payload without referencing one.");
        }
        if (heldPayload->length > MaximumPayloadLength ||
            heldPayload->length > context.scope.desktopControl.maxTypedCharacters)
        {
            return Refuse(SubgoalRejection::OversizedField,
                "The referenced content is longer than this task may type.");
        }
    }

    // Completion conditions, each checked for relevance to this subgoal's own target.
    for (const SubgoalPostcondition& condition : proposed.postconditions)
    {
        if (condition.payload.Valid() && !vault.Holds(condition.payload))
        {
            return Refuse(SubgoalRejection::UnknownPayload,
                "A completion condition referenced content the runtime is not holding.");
        }
        if (!FieldAcceptable(condition.subject, MaximumDescriptorField) ||
            !FieldAcceptable(condition.value, MaximumDescriptorField))
        {
            return Refuse(SubgoalRejection::OversizedField,
                "A completion condition carried a field longer than the contract allows.");
        }
        if (!PostconditionIsRelevant(condition, proposed))
        {
            return Refuse(SubgoalRejection::UnverifiablePostcondition,
                "A completion condition asked about something other than what this "
                "subgoal was asked to do.");
        }
    }

    // Accepted. What comes back is the proposal with the runtime's own facts stamped
    // onto it -- origin, scope and budget are read from the run and never from the
    // proposal, so a proposal cannot describe its own authority.
    SubgoalValidation validation;
    validation.accepted = true;
    validation.subgoal = proposed;
    // The runtime's description of the payload, not the proposal's.
    if (heldPayload.has_value()) validation.subgoal.payload = *heldPayload;
    validation.subgoal.goalId = context.goalId;
    validation.subgoal.origin = context.origin;
    validation.subgoal.scope = context.scope;
    validation.subgoal.actionsLeft = context.actionsLeft;
    validation.subgoal.retriesLeft = context.retriesLeft;
    validation.subgoal.authority.granted = true;
    return validation;
}

} // namespace revia::computer
