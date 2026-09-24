#include "testSupport.h"

#include "Computer/taskProgression.h"
#include "Computer/payloadVault.h"
#include "Computer/subgoalValidator.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// The state machine that decides what kind of operation comes next.
//
// This exists because a 4B Main could produce valid, well-formed subgoals and still
// choose the wrong operation for the state the task was in: `resolve_target` on a task
// whose content was already in the vault, "Send" on a task whose content had not been
// typed, and a bare edit field on a task that named the Compose panel. None of those is
// a wording problem. Each is a question the runtime already knew the answer to and asked
// anyway.
//
// So the tests below are about the division of labour rather than about any model. Where
// task state determines the operation, nothing is asked. Where meaning is genuinely
// ambiguous, something is.

const std::string Message = "dinner at eight";
const std::string Application = "reviadesktopfixture.exe";

ObservedCandidate Edit(
    const std::string& id,
    const std::string& name,
    const std::string& container = {},
    const std::string& label = {})
{
    ObservedCandidate candidate;
    candidate.id = id;
    candidate.name = name;
    candidate.container = container;
    candidate.inferredLabel = label;
    candidate.role = "edit";
    candidate.nameless = name.empty();
    candidate.maySetText = true;
    candidate.mayType = true;
    return candidate;
}

ObservedCandidate Button(const std::string& id, const std::string& name)
{
    ObservedCandidate candidate;
    candidate.id = id;
    candidate.name = name;
    candidate.role = "button";
    candidate.mayInvoke = true;
    return candidate;
}

ComputerTaskContext Screen(
    std::vector<ObservedCandidate> candidates,
    const std::string& foreground = Application)
{
    ComputerTaskContext context;
    context.observation.screen.succeeded = true;
    context.observation.screen.foregroundApplication = foreground;
    context.observation.screen.foregroundTitle = "Revia Fixture - Main";
    context.observation.candidates = std::move(candidates);
    context.actionsLeft = 10;
    context.retriesLeft = 2;
    return context;
}

TaskProgressInputs Inputs(
    const TaskContent& content,
    const ComputerTaskContext& context,
    const PayloadReference& payload,
    const bool placed)
{
    TaskProgressInputs inputs;
    inputs.content = &content;
    inputs.context = &context;
    inputs.payload = payload;
    inputs.contentPlaced = placed;
    inputs.submissionReady = placed;
    inputs.application = Application;
    inputs.goalId = "goal-1";
    return inputs;
}

// ---- the phases the runtime owns ----

void TestAWindowThatIsNotInFrontIsBroughtForwardWithoutAsking()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" in the Compose box");
    const ComputerTaskContext context =
        Screen({Edit("c1", "Compose")}, "someoneelse.exe");
    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, {}, false));

    Check(progress.phase == TaskPhase::AcquireWindow,
        "The runtime did not notice that the application it is supposed to act in is "
        "not the one in front.");
    Check(progress.derivable,
        "Bringing a window forward was treated as a question for a model. Nothing about "
        "it is ambiguous.");
    Check(progress.proposed.intent == SubgoalIntent::FocusWindow &&
            progress.proposed.target.application == Application,
        "The derived subgoal does not focus the application the task is about.");
}

void TestAnIdentifiedDestinationGoesStraightToPlacement()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" in the Compose box");
    PayloadVault vault;
    const PayloadReference payload = vault.Store(Message, "message");
    const ComputerTaskContext context =
        Screen({Edit("c1", "Compose"), Edit("s1", "Subject"), Button("b1", "Send")});

    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, payload, false));

    Check(progress.phase == TaskPhase::PlaceContent,
        "With the content held and exactly one field answering to the name the person "
        "used, the runtime still did not know the next operation was to place it. This "
        "is the decision Main was getting wrong.");
    Check(progress.derivable, "Placement was treated as a question for a model.");
    Check(progress.proposed.intent == SubgoalIntent::EnterPayload,
        "The derived operation was not entering the payload.");
    Check(progress.proposed.payload.id == payload.id,
        "The derived subgoal did not reference the held content.");
    Check(progress.proposed.target.name == "compose",
        "The derived subgoal did not name the field the person named.");
}

// An unnamed field inside a named panel. The same phrase, resolved by different
// evidence, and it must still not need a model.
void TestAnUnnamedFieldInANamedPanelIsAlsoDerivable()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" in the Compose box");
    PayloadVault vault;
    const PayloadReference payload = vault.Store(Message, "message");
    const ComputerTaskContext context = Screen({
        Edit("c1", "", "Compose"),
        Edit("s1", "", "Subject"),
    });

    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, payload, false));

    Check(progress.phase == TaskPhase::PlaceContent && progress.derivable,
        "A field identified by the panel it sits in was not resolvable without a model, "
        "which is exactly the case ISSUE-REVIA-0070 opened up.");
    Check(progress.proposed.target.container == "compose" &&
            progress.proposed.target.name.empty(),
        "The derived descriptor did not reach the field by its container, so the "
        "matcher's container tier will not find it.");
}

// ---- the phase the model is actually for ----

void TestAnAmbiguousDestinationIsTheQuestionAModelGetsAsked()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" in the Compose box");
    PayloadVault vault;
    const PayloadReference payload = vault.Store(Message, "message");
    // Two fields answer to the same description: one named Compose, one in a panel
    // called Compose. A person would ask which; so does the runtime.
    const ComputerTaskContext context = Screen({
        Edit("c1", "Compose"),
        Edit("c2", "Compose"),
    });

    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, payload, false));

    Check(progress.phase == TaskPhase::ResolveDestination,
        "Two fields answering to one description did not produce a resolution phase.");
    Check(!progress.derivable,
        "The runtime derived an operation for an ambiguous destination. Picking one of "
        "two equally good matches is a guess, and this is the one place a model earns "
        "its call.");
    Check(progress.destinationAmbiguous && !progress.destinationMissing,
        "The refusal did not distinguish 'two things match' from 'nothing matches'. "
        "They are different questions and want different help.");
}

void TestADestinationThatIsNotThereIsAlsoAQuestion()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" in the Attachments box");
    const ComputerTaskContext context = Screen({Edit("c1", "Compose")});
    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, {}, false));

    Check(progress.phase == TaskPhase::ResolveDestination && !progress.derivable,
        "A destination nothing on screen answers to was acted on anyway.");
    Check(progress.destinationMissing && !progress.destinationAmbiguous,
        "A missing destination was reported as an ambiguous one.");
}

void TestARequestThatNamesNoFieldAsksRatherThanChoosing()
{
    const TaskContent content = ExtractTaskContent("type \"dinner at eight\"");
    const ComputerTaskContext context =
        Screen({Edit("c1", "Compose"), Edit("s1", "Subject")});
    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, {}, false));

    Check(progress.phase == TaskPhase::ResolveDestination && !progress.derivable,
        "A request that named no field had one chosen for it. With two editable fields "
        "on screen that is a coin toss with someone's words in it.");
}

// ---- submission ----

void TestSubmissionIsNotAvailableWhileContentIsPending()
{
    const TaskContent content =
        ExtractTaskContent("send \"dinner at eight\" in the Compose box");
    Check(content.submissionRequested,
        "The request asked for the content to be sent and the parse did not see it.");

    PayloadVault vault;
    const PayloadReference payload = vault.Store(Message, "message");
    const ComputerTaskContext context =
        Screen({Edit("c1", "Compose"), Button("b1", "Send")});

    const TaskProgress pending =
        DeriveTaskProgress(Inputs(content, context, payload, /*placed=*/false));
    Check(pending.phase == TaskPhase::PlaceContent,
        "A task that asked for sending reached a phase other than placement while its "
        "content was still pending. There must be no route from pending content to "
        "submission -- that is what makes the ordering a property of the state machine "
        "rather than a rule somebody has to remember.");
    Check(pending.proposed.intent == SubgoalIntent::EnterPayload,
        "The operation derived while content was pending was not placement.");
}

void TestSubmissionBecomesAvailableOnlyAfterVerifiedPlacement()
{
    const TaskContent content =
        ExtractTaskContent("send \"dinner at eight\" in the Compose box");
    PayloadVault vault;
    const PayloadReference payload = vault.Store(Message, "message");
    const ComputerTaskContext context =
        Screen({Edit("c1", "Compose"), Button("b1", "Send")});

    const TaskProgress placed =
        DeriveTaskProgress(Inputs(content, context, payload, /*placed=*/true));
    Check(placed.phase == TaskPhase::Submit && placed.derivable,
        "With the content verified in the field and one control answering to the word "
        "the person used, submission was still not derivable.");
    Check(placed.proposed.intent == SubgoalIntent::InteractWithControl &&
            placed.proposed.target.name == "send",
        "The derived submission did not aim at the control the person named.");
    auto stale = Inputs(content, context, payload, true);
    stale.submissionReady = false;
    Check(!DeriveTaskProgress(stale).derivable,
        "Historical placement without current draft verification derived a submission.");
    stale.contentPlaced = false;
    stale.submissionDone = true;
    Check(DeriveTaskProgress(stale).phase == TaskPhase::Complete,
        "A completed submission that cleared its draft would be placed and sent again.");
}

// The refusal that protects the case the live run actually produced.
void TestADerivedSubmissionStillFacesTheOrderingRule()
{
    PayloadVault vault;
    const PayloadReference payload = vault.Store(Message, "message");

    ComputerSubgoal proposed;
    proposed.schemaVersion = CurrentSubgoalSchema;
    proposed.intent = SubgoalIntent::InteractWithControl;
    proposed.target.application = Application;
    proposed.target.name = "Send";

    SubgoalContext context;
    context.goalId = "goal-1";
    context.origin = RequestOrigin::UserDirected;
    context.scope.approvedApplications = {Application};
    context.scope.desktopControl.keyboard = true;
    context.scope.desktopControl.pointer = true;
    context.actionsLeft = 8;
    context.retriesLeft = 2;
    // The state the derivation must never produce -- and if a bug ever produced it, the
    // validator is the second line that refuses it.
    context.contentPending = true;

    const SubgoalValidation refused = ValidateSubgoal(proposed, context, vault);
    Check(!refused.accepted &&
            refused.rejection == SubgoalRejection::SendBeforePlacement,
        "A submission proposed while content was pending was accepted. The derivation "
        "should never produce this, and the validator must refuse it if it ever does.");
}

void TestATaskAsksNothingFurtherOnceItsContentHasLanded()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" in the Compose box. Do not send anything.");
    Check(!content.submissionRequested,
        "A request that explicitly refused sending was read as asking for it. This is "
        "the one parse error here that cannot be undone by a later step.");

    PayloadVault vault;
    const PayloadReference payload = vault.Store(Message, "message");
    const ComputerTaskContext context =
        Screen({Edit("c1", "Compose"), Button("b1", "Send")});

    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, payload, /*placed=*/true));
    Check(progress.phase == TaskPhase::Complete,
        "A placement task whose content had landed did not read as finished, so the run "
        "would pay a model call to be told what the runtime already knew.");
    Check(!progress.derivable,
        "Completion produced a subgoal. There is nothing left to do.");
}

// ---- what the runtime refuses to derive ----

void TestNothingIsDerivedFromAnUnreadableScreen()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" in the Compose box");
    ComputerTaskContext context = Screen({Edit("c1", "Compose")});
    context.observation.screen.succeeded = false;

    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, {}, false));
    Check(progress.phase == TaskPhase::Undetermined && !progress.derivable,
        "An operation was derived from a screen that could not be read. That is "
        "deciding on the basis of not having looked.");
}

void TestATaskWithNoIdentifiableContentHasNoDerivedProgression()
{
    const TaskContent content = ExtractTaskContent("press the Zoom in button");
    const ComputerTaskContext context = Screen({Button("b1", "Zoom in")});
    const TaskProgress progress =
        DeriveTaskProgress(Inputs(content, context, {}, false));

    Check(progress.phase == TaskPhase::Undetermined,
        "A state machine was invented for a task that does not have one. Tasks with no "
        "identifiable content go to the model exactly as they always did.");
}

} // namespace

void RunTaskProgressionTests()
{
    TestAWindowThatIsNotInFrontIsBroughtForwardWithoutAsking();
    TestAnIdentifiedDestinationGoesStraightToPlacement();
    TestAnUnnamedFieldInANamedPanelIsAlsoDerivable();
    TestAnAmbiguousDestinationIsTheQuestionAModelGetsAsked();
    TestADestinationThatIsNotThereIsAlsoAQuestion();
    TestARequestThatNamesNoFieldAsksRatherThanChoosing();
    TestSubmissionIsNotAvailableWhileContentIsPending();
    TestSubmissionBecomesAvailableOnlyAfterVerifiedPlacement();
    TestADerivedSubmissionStillFacesTheOrderingRule();
    TestATaskAsksNothingFurtherOnceItsContentHasLanded();
    TestNothingIsDerivedFromAnUnreadableScreen();
    TestATaskWithNoIdentifiableContentHasNoDerivedProgression();

    std::cout << "The runtime decides which operation comes next when task state settles it, "
                 "and asks a\nmodel only when the screen is genuinely ambiguous. Submission "
                 "has no route from pending content.\n";
}
