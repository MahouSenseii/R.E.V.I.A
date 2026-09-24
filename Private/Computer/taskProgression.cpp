#include "Computer/taskProgression.h"

#include "Computer/targetMatch.h"

#include <algorithm>
#include <cctype>

namespace revia::computer
{

namespace
{

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

// Two executables naming the same application. Compared the way the scope compares them,
// because a progression that disagreed with the scope about which window it is looking at
// would derive an operation for the wrong one.
bool SameApplication(const std::string& left, const std::string& right)
{
    if (left.empty() || right.empty()) return false;
    return Lowered(left) == Lowered(right);
}

// The destination, asked for twice in the order the evidence deserves.
//
// A person writes "the Compose box". That may be a control the application named
// "Compose", or a control UI Automation infers is labelled "Compose", or an unnamed edit
// sitting inside a panel called "Compose" -- and those are three different descriptors,
// because the matcher's tiers are keyed on which field is filled.
//
// Named first. A published name is what the application asserted about itself, and a
// container is only where a thing sits; letting the second answer while the first could
// have would be preferring the weaker evidence. Ambiguity in the first attempt stops
// there rather than falling through, because "two things are called Compose" is already
// the question, and looking somewhere else for a third is not an answer to it.
TargetMatch ResolveDestination(
    const std::vector<ObservedCandidate>& candidates,
    const std::string& destination,
    TargetDescriptor& outDescriptor)
{
    TargetDescriptor byName;
    byName.name = destination;
    const TargetMatch named =
        MatchTarget(candidates, byName, TargetAffordance::Editable);
    if (named.outcome != TargetMatch::Outcome::NotFound)
    {
        outDescriptor = byName;
        return named;
    }

    TargetDescriptor byContainer;
    byContainer.container = destination;
    byContainer.role = "edit";
    const TargetMatch contained =
        MatchTarget(candidates, byContainer, TargetAffordance::Editable);
    outDescriptor = byContainer;
    return contained;
}

} // namespace

std::string ToString(const TaskPhase value)
{
    switch (value)
    {
        case TaskPhase::AcquireWindow: return "acquire_window";
        case TaskPhase::ResolveDestination: return "resolve_destination";
        case TaskPhase::PlaceContent: return "place_content";
        case TaskPhase::Submit: return "submit";
        case TaskPhase::Complete: return "complete";
        case TaskPhase::Undetermined:
        default: return "undetermined";
    }
}

TaskProgress DeriveTaskProgress(const TaskProgressInputs& inputs)
{
    TaskProgress progress;
    if (inputs.content == nullptr || inputs.context == nullptr) return progress;
    progress.contentPlaced = inputs.contentPlaced;

    const TaskContent& content = *inputs.content;
    const ComputerTaskContext& context = *inputs.context;

    // A task with nothing identifiable to place has no progression the runtime can
    // derive, and pretending otherwise would be inventing a state machine for tasks that
    // do not have one. Those go to the model exactly as before.
    if (!content.Any()) return progress;

    // Submission can empty the draft. Its execution still ends the task; requiring
    // the old body to remain visible here would place and send it a second time.
    if (content.submissionRequested && inputs.submissionDone)
    {
        progress.phase = TaskPhase::Complete;
        progress.detail = "The submission has already been made.";
        return progress;
    }

    // Nor can anything be derived from a screen that could not be read. An unreadable
    // observation is an absence of evidence; deriving an operation from it would be
    // deciding on the basis of not having looked.
    if (!context.observation.Available()) return progress;

    const std::string& application = inputs.application;
    if (application.empty()) return progress;

    const auto propose = [&](const SubgoalIntent intent, const TargetDescriptor& target)
    {
        ComputerSubgoal subgoal;
        subgoal.schemaVersion = CurrentSubgoalSchema;
        subgoal.intent = intent;
        subgoal.target = target;
        subgoal.target.application = application;
        return subgoal;
    };

    // ---- 1. the application has to be in front ----
    //
    // Not a question. A control in a window that is not in front is a control this task
    // cannot act on, and no model needs to be asked whether to bring it forward.
    if (!SameApplication(context.observation.screen.foregroundApplication, application))
    {
        TargetDescriptor window;
        progress.phase = TaskPhase::AcquireWindow;
        progress.derivable = true;
        progress.proposed = propose(SubgoalIntent::FocusWindow, window);
        progress.detail = "Bring " + application + " to the front.";
        return progress;
    }

    // ---- 2 and 3. resolve the destination, then place the content ----
    if (!inputs.contentPlaced)
    {
        // A request that named no destination leaves nothing to resolve deterministically.
        // That is a question about meaning, which is what the model is for.
        if (content.destination.empty())
        {
            progress.phase = TaskPhase::ResolveDestination;
            progress.destinationMissing = true;
            progress.detail =
                "The request did not say which field the content goes in.";
            return progress;
        }

        TargetDescriptor descriptor;
        const TargetMatch match = ResolveDestination(
            context.observation.candidates, content.destination, descriptor);

        if (match.outcome == TargetMatch::Outcome::Ambiguous)
        {
            // Two things answer to the same description. Picking one is a guess wearing
            // a decision's clothes, and this is precisely the case a model can help with
            // -- so it is the one case that costs a call.
            progress.phase = TaskPhase::ResolveDestination;
            progress.destinationAmbiguous = true;
            progress.detail = "More than one field answers to \"" +
                content.destination + "\".";
            return progress;
        }
        if (match.outcome == TargetMatch::Outcome::NotFound)
        {
            progress.phase = TaskPhase::ResolveDestination;
            progress.destinationMissing = true;
            progress.detail = "Nothing on screen answers to \"" +
                content.destination + "\".";
            return progress;
        }

        // Exactly one editable field answers to what the person called it. The operation
        // that follows is not in doubt.
        progress.phase = TaskPhase::PlaceContent;
        progress.derivable = true;
        progress.proposed = propose(SubgoalIntent::EnterPayload, descriptor);
        progress.proposed.payload = inputs.payload;
        progress.detail = "Put the prepared content in " +
            match.candidate->Describe() + ".";
        return progress;
    }

    // ---- 4 is the runner's. 5, 6 and 7 are below ----
    //
    // Reaching this point means the content has been seen in the field and verified by a
    // typed postcondition. There is no route here from pending content, which is what
    // makes "no submission before verified placement" a property of the state machine
    // rather than a rule somebody has to remember to check.
    if (!content.submissionRequested)
    {
        // The request asked for placement and placement has happened. Submitting would
        // be doing something nobody asked for, and asking a model whether to would be
        // offering it the chance to say yes.
        progress.phase = TaskPhase::Complete;
        progress.derivable = false;
        progress.detail = "The content is in the field and the request asked for "
            "nothing further.";
        return progress;
    }

    if (inputs.submissionDone)
    {
        // It has been pressed. Whether the effect could be confirmed is a question for
        // the report, not for whether to press it again -- and pressing it again is how
        // one message becomes two.
        progress.phase = TaskPhase::Complete;
        progress.derivable = false;
        progress.detail = "The content was placed and the submission has been made.";
        return progress;
    }

    if (!inputs.submissionReady)
    {
        progress.phase = TaskPhase::Submit;
        progress.detail = "The draft must be revalidated in its original context before submission.";
        return progress;
    }

    // The submission control, by the word the person used for it. Invokable, because
    // submitting is pressing something; and exactly one, because "Send" must not press
    // "Send later".
    TargetDescriptor submit;
    submit.name = content.submissionVerb;
    const TargetMatch match = MatchTarget(
        context.observation.candidates, submit, TargetAffordance::Invokable);
    if (match.outcome != TargetMatch::Outcome::Found)
    {
        progress.phase = TaskPhase::Submit;
        progress.derivable = false;
        progress.destinationAmbiguous =
            match.outcome == TargetMatch::Outcome::Ambiguous;
        progress.destinationMissing = match.outcome == TargetMatch::Outcome::NotFound;
        progress.detail = match.outcome == TargetMatch::Outcome::Ambiguous
            ? "More than one control could submit this."
            : "Nothing on screen obviously submits this.";
        return progress;
    }

    progress.phase = TaskPhase::Submit;
    progress.derivable = true;
    progress.proposed = propose(SubgoalIntent::InteractWithControl, submit);
    progress.detail = "Use " + match.candidate->Describe() + ".";
    return progress;
}

} // namespace revia::computer
