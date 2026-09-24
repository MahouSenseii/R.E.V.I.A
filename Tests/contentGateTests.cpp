#include "testSupport.h"

#include "Computer/contentGate.h"
#include "Computer/payloadVault.h"
#include "Computer/taskContent.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// The exact-content contract, exercised without a model, a desktop or a session.
//
// The defect these are about was observed on a live backend and was invisible to every
// scripted test in the suite: asked to place a message, Main wrote "The prepared message
// text here" into the box and reported the task complete. Nothing was broken in the sense
// a test usually means -- the step ran, the check passed, the goal succeeded. What was
// missing was anyone holding the real sentence to compare against.
//
// So these tests are written from the outside in: not "does the gate set the field" but
// "can a planner's words reach the machine", which is the question that was answered
// wrongly for so long.

const std::string Message = "dinner at eight";

ObservedCandidate Field(
    const std::string& id,
    const std::string& name,
    const std::string& container = {})
{
    ObservedCandidate candidate;
    candidate.id = id;
    candidate.name = name;
    candidate.container = container;
    candidate.role = "edit";
    candidate.nameless = name.empty();
    candidate.maySetText = true;
    candidate.mayType = true;
    return candidate;
}

ComputerTaskContext Screen(std::vector<ObservedCandidate> candidates)
{
    ComputerTaskContext context;
    context.observation.screen.succeeded = true;
    context.observation.screen.foregroundApplication = "reviadesktopfixture.exe";
    context.observation.candidates = std::move(candidates);
    return context;
}

// A step shaped the way the legacy path produces one: the value already written by a
// model, because that grammar offered a free string for it.
revia::goals::GoalStep LegacyEntry(const std::string& control, const std::string& value)
{
    revia::goals::GoalStep step;
    step.action.type = revia::actions::ActionType::SetControlText;
    step.action.application = "reviadesktopfixture.exe";
    step.action.control = control;
    step.action.value = value;
    step.check.type = revia::actions::ActionType::InspectWindow;
    step.check.application = "reviadesktopfixture.exe";
    return step;
}

TaskContent Exact(const std::string& value, const std::string& destination = {})
{
    TaskContent content;
    content.requirement = ContentRequirement::ExactUserContent;
    content.value = value;
    content.kind = "message";
    content.destination = destination;
    return content;
}

// ---- what the runtime reads out of the request ----

void TestTheUsersOwnWordsAreLiftedFromTheRequest()
{
    const TaskContent content =
        ExtractTaskContent("put \"dinner at eight\" into the Compose box in the fixture window");
    Check(content.requirement == ContentRequirement::ExactUserContent,
        "A quoted span in the request was not recognised as the content to place, so "
        "the task would have been planned with nothing in custody.");
    Check(content.value == Message,
        "The extracted content was not the user's exact words: '" + content.value + "'.");
    Check(content.destination == "compose",
        "The field the user named was not read out of the request: '" +
            content.destination + "'.");
    // Nothing in that sentence says "message", so the honest label is the general one.
    // A parser that called every quoted span a message would be guessing, and the kind
    // exists so a policy can reason without being shown the value -- which only works
    // while it is a fact rather than an assumption.
    Check(content.kind == "text",
        "An unqualified quoted span was labelled something more specific than the "
        "request supports: '" + content.kind + "'.");

    const TaskContent named =
        ExtractTaskContent("send the message \"dinner at eight\" in Compose");
    Check(named.kind == "message",
        "A request that said the word message did not produce that kind, so a policy "
        "cannot tell a message from a filename without being shown one.");
}

// "put 'meet me in the lobby' into Compose" names Compose, and a destination search
// over the whole sentence would name the lobby.
void TestTheDestinationIsReadAfterTheContentAndNotInsideIt()
{
    const TaskContent content =
        ExtractTaskContent("type \"meet me in the lobby\" into the Subject line");
    Check(content.value == "meet me in the lobby",
        "The quoted span was not extracted whole.");
    Check(content.destination == "subject",
        "The destination was taken from inside the user's own words rather than from "
        "the instruction around them: '" + content.destination + "'.");
}

void TestAnApostropheIsNotAQuotedSpan()
{
    const TaskContent content =
        ExtractTaskContent("tell them I won't be late and I'll be there soon");
    Check(content.requirement != ContentRequirement::ExactUserContent,
        "An apostrophe was read as a quote, which would have placed the fragment "
        "between two of them into somebody's message box.");

    // And the other half: a single quote that really is one still works, so the guard
    // above is telling punctuation from spelling rather than giving up on the mark.
    const TaskContent quoted = ExtractTaskContent("put 'dinner at eight' in Compose");
    Check(quoted.requirement == ContentRequirement::ExactUserContent &&
            quoted.value == Message,
        "A genuine single-quoted span was refused along with the apostrophes: '" +
            quoted.value + "'.");
}

void TestCurlyQuotesAreRecognised()
{
    // Split at every hex escape on purpose: "\x9C" followed by a letter is one hex
    // escape in C++, not an escape and a letter, and the munched value is out of range.
    const TaskContent content = ExtractTaskContent(
        "put \xE2\x80\x9C" "dinner at eight" "\xE2\x80\x9D" " in Compose");
    Check(content.requirement == ContentRequirement::ExactUserContent,
        "Curly quotes were not recognised, so a request pasted out of a chat client "
        "would fall through to having no content at all.");
    Check(content.value == Message, "The curly-quoted span was not extracted whole.");
}

void TestADraftingRequestIsContentToComposeRatherThanContentToPlace()
{
    const TaskContent content =
        ExtractTaskContent("compose a short message to my sister in the Compose box");
    Check(content.requirement == ContentRequirement::Drafted,
        "A request to compose was not distinguished from a request to place exact "
        "words, and the two need opposite treatment.");
    Check(content.value.empty(),
        "A drafting request arrived carrying a value, which cannot exist yet.");
}

void TestARequestWithNoIdentifiableContentIsLeftAlone()
{
    const TaskContent content = ExtractTaskContent("press the Zoom in button");
    Check(content.requirement == ContentRequirement::None,
        "A task with nothing to type was given a content requirement, which would "
        "refuse ordinary work that never had a payload.");
}

// ---- what may reach a field ----

void TestAPlannersInventedTextNeverReachesTheField()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(Exact(Message, "compose"));

    ComputerTaskContext context = Screen({Field("c1", "Compose")});
    // Exactly what the live run produced. The words are plausible, well-formed, and
    // not the user's.
    revia::goals::GoalStep step = LegacyEntry("c1", "The prepared message text here");
    const ContentDecision decision = gate.Apply(step, context);

    Check(decision.allowed,
        "The step was refused outright. The destination was right and the content was "
        "held, so the repair is to place the real words, not to fail the task.");
    Check(decision.outcome == ContentOutcome::InventedText,
        "A planner writing its own sentence was not recorded as an invention, so the "
        "one measurement that says whether this keeps happening is not being taken.");
    Check(step.action.value == Message,
        "The invented text reached the field: '" + step.action.value + "'. This is the "
        "defect exactly as it was observed against a live model.");
    Check(gate.Stats().inventions == 1, "The invention was not counted.");
}

// The step's description is rewritten to name the field, not the content.
//
// Found by a live run: a planner told to write a fixed token where the words go writes
// it in `expected` too, and verification then rules the typed condition irrelevant and
// falls back to a substring search for the token. A step that did exactly the right
// thing came back "could not establish".
void TestTheStepDescriptionNamesTheFieldRatherThanTheContent()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(Exact(Message, "compose"));

    ComputerTaskContext context = Screen({Field("c1", "Compose")});
    revia::goals::GoalStep step = LegacyEntry("c1", PreparedContentToken);
    step.expected = PreparedContentToken;
    static_cast<void>(gate.Apply(step, context));

    Check(step.expected.find(PreparedContentToken) == std::string::npos,
        "The placeholder survived into the step's description, so verification will "
        "search the check output for a token that can never appear in it.");
    Check(step.expected.find("c1") != std::string::npos,
        "The description does not name the control, so the typed postcondition will be "
        "judged irrelevant and the step will fall back to the substring rule.");
    Check(step.expected.find(Message) == std::string::npos,
        "The user's own words were written into the step description, which is one of "
        "the places this whole arrangement exists to keep them out of.");
}

void TestAnAlteredCopyOfTheContentIsReplacedAndCounted()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(Exact(Message, "compose"));

    ComputerTaskContext context = Screen({Field("c1", "Compose")});
    // A planner that tried to reproduce the content and got it slightly wrong. This is
    // more dangerous than an obvious placeholder, not less: it survives a human glance.
    revia::goals::GoalStep step = LegacyEntry("c1", "Dinner at eight!");
    const ContentDecision decision = gate.Apply(step, context);

    Check(decision.outcome == ContentOutcome::ModifiedPayload,
        "A near-copy of the content was not distinguished from an invention. Both are "
        "repaired the same way and they say different things about the planner.");
    Check(step.action.value == Message,
        "An altered copy of the user's words was placed: '" + step.action.value + "'.");
    Check(gate.Stats().modifiedPayloads == 1, "The alteration was not counted.");
}

void TestContentIsRefusedWhenTheFieldIsNotTheOneAsked()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(Exact(Message, "compose"));

    ComputerTaskContext context =
        Screen({Field("c1", "Compose"), Field("s1", "Subject")});
    revia::goals::GoalStep step = LegacyEntry("s1", "");
    const ContentDecision decision = gate.Apply(step, context);

    Check(!decision.allowed,
        "The content was allowed into a field the user did not name. The right words "
        "in the wrong box is the same mistake as the wrong words in the right one.");
    Check(decision.outcome == ContentOutcome::WrongDestination,
        "The refusal did not say what was wrong with it.");
    Check(step.action.value.empty(),
        "A refused step still came away carrying the user's content.");
    Check(gate.Stats().wrongDestinations == 1, "The wrong field was not counted.");
}

// A control the observation does not describe is one this cannot judge. Refusing on a
// listing that was merely capped would stop tasks that are entirely correct.
void TestAnUnlistedControlIsNotTreatedAsTheWrongField()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(Exact(Message, "compose"));

    ComputerTaskContext context = Screen({Field("c1", "Compose")});
    revia::goals::GoalStep step = LegacyEntry("not-in-the-listing", "");
    const ContentDecision decision = gate.Apply(step, context);

    Check(decision.allowed,
        "A control the observation did not list was treated as a mismatch, which turns "
        "a capped listing into a refused task.");
}

void TestAMissingPayloadAsksRatherThanInventing()
{
    PayloadVault vault;
    ContentGate gate(vault);
    // The user asked for exact content and the extraction found none to take custody
    // of -- an empty quoted span, a value too long to hold, a request that named a
    // message it did not contain.
    TaskContent content;
    content.requirement = ContentRequirement::ExactUserContent;
    content.kind = "message";
    gate.BeginTask(content);

    ComputerTaskContext context = Screen({Field("c1", "Compose")});
    revia::goals::GoalStep step = LegacyEntry("c1", "something plausible");
    const ContentDecision decision = gate.Apply(step, context);

    Check(!decision.allowed && decision.needsInput,
        "A task that needs content it was never given did not stop to ask. The "
        "alternative it was taking is typing a plausible substitute.");
    Check(decision.outcome == ContentOutcome::MissingPayload,
        "The refusal was not recorded as a missing payload.");
    Check(step.action.value != "something plausible" || !decision.allowed,
        "The step was allowed through carrying text nobody supplied.");
}

// The token is the grammar's way of saying "the runtime supplies this". Typing it is
// the single worst outcome available here, so it is refused even on the path where
// nothing else is checked.
void TestThePlaceholderTokenIsNeverTyped()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(TaskContent{});

    ComputerTaskContext context = Screen({Field("c1", "Compose")});
    revia::goals::GoalStep step = LegacyEntry("c1", PreparedContentToken);
    const ContentDecision decision = gate.Apply(step, context);

    Check(!decision.allowed && decision.needsInput,
        "The literal placeholder was allowed into a field. It would have been typed "
        "and then verified, because the check reads back whatever the action wrote.");
}

void TestADraftIsFixedOnceItIsChosen()
{
    PayloadVault vault;
    ContentGate gate(vault);
    TaskContent content;
    content.requirement = ContentRequirement::Drafted;
    content.kind = "message";
    gate.BeginTask(content);

    ComputerTaskContext context = Screen({Field("c1", "Compose")});
    revia::goals::GoalStep first = LegacyEntry("c1", "Running late, see you at eight");
    const ContentDecision adopted = gate.Apply(first, context);
    Check(adopted.outcome == ContentOutcome::DraftAdopted && adopted.allowed,
        "Composing was refused for a task whose whole point was to compose.");
    Check(gate.Held().provenance == ContentProvenance::Drafted,
        "The draft was taken into custody without recording that she wrote it, so it "
        "would later be indistinguishable from the user's own words.");

    // The retry. A value still living in a planner's head is free to come back
    // different, and the check would then be reading one sentence and the attempt
    // writing another.
    revia::goals::GoalStep second = LegacyEntry("c1", "Running a bit late, see you soon");
    const ContentDecision again = gate.Apply(second, context);
    Check(second.action.value == "Running late, see you at eight",
        "A second attempt rewrote the draft: '" + second.action.value + "'. The value "
        "a step is verified against has to be the value it typed.");
    // Recorded as a fresh invention rather than an alteration, because it shares
    // nothing with the sentence already in custody. The label follows the evidence: a
    // near-copy says the planner was trying to reproduce the value, and a different
    // sentence says it was writing a new one.
    Check(again.outcome == ContentOutcome::InventedText &&
            gate.Stats().inventions == 1,
        "Rewriting an adopted draft was not recorded at all, so a draft that changed "
        "between the attempt and the check would leave no trace.");
}

// ---- what may be called finished ----

revia::goals::Goal GoalWith(revia::goals::GoalStep step, const bool verified)
{
    revia::goals::StepAttempt attempt;
    attempt.executed = true;
    attempt.verified = verified;
    attempt.outcome = verified ? revia::goals::VerificationOutcome::Verified
                               : revia::goals::VerificationOutcome::Unknown;
    step.attempts.push_back(attempt);

    revia::goals::Goal goal;
    goal.steps.push_back(std::move(step));
    return goal;
}

revia::actions::windows::DraftSnapshot Draft(const std::string& value)
{
    revia::actions::windows::DraftSnapshot draft;
    draft.valid = true;
    draft.value = value;
    draft.context = "recipient=alice";
    draft.control.valid = true;
    draft.control.window = reinterpret_cast<void*>(0x1);
    draft.control.processId = 23;
    draft.control.runtimeId = "compose-field";
    draft.control.controlType = 50004;
    draft.control.automationId = "c1";
    draft.control.contextFingerprint = "mail-to-alice";
    return draft;
}

void TestCompletionIsRefusedUntilTheContentIsSeenInTheField()
{
    PayloadVault vault;
    ContentGate gate(vault, [](const auto&) { return Draft(Message); });
    gate.BeginTask(Exact(Message, "compose"));

    Check(!gate.CompletionAllowed(),
        "A task was allowed to report itself finished before its content went "
        "anywhere. This is the second half of the defect: typed something, declared "
        "victory.");

    // The step that entered the wrong text. It was repaired on the way through, so
    // what is on the goal is the real content -- and it has not been verified yet.
    revia::goals::GoalStep step = LegacyEntry("c1", Message);
    gate.ObserveGoal(GoalWith(step, /*verified=*/false));
    Check(!gate.CompletionAllowed(),
        "An entry that ran but could not be checked was counted as a placement. An "
        "open question is not evidence.");

    gate.ObserveGoal(GoalWith(step, /*verified=*/true));
    Check(gate.CompletionAllowed(),
        "A verified placement of the exact content was still not enough to finish.");
}

// The case the user's report actually described: text was entered, it was the wrong
// text, and the run reported success. The repair has to hold even when the wrong text
// was what got verified.
void TestEnteringIncorrectTextIsNotAPlacement()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(Exact(Message, "compose"));

    revia::goals::GoalStep invented = LegacyEntry("c1", "The prepared message text here");
    // Verified -- because a check that reads back whatever the action wrote will
    // confirm an invented sentence as readily as a real one.
    gate.ObserveGoal(GoalWith(invented, /*verified=*/true));

    Check(!gate.CompletionAllowed(),
        "A verified entry of text nobody asked for satisfied the task. The check "
        "confirmed the field held what was typed, which was never the question.");
}

void TestATaskWithNothingToPlaceFinishesAsItAlwaysDid()
{
    PayloadVault vault;
    ContentGate gate(vault);
    gate.BeginTask(TaskContent{});
    Check(gate.CompletionAllowed(),
        "A task with no identifiable content was blocked from finishing, which would "
        "break every task that never had a payload.");
}

void TestHistoricalPlacementCannotAuthorizeSubmission()
{
    PayloadVault vault;
    ContentGate gate(vault);
    TaskContent task = Exact(Message, "compose");
    task.submissionRequested = true;
    task.submissionVerb = "send";
    gate.BeginTask(task);
    gate.ObserveGoal(GoalWith(LegacyEntry("c1", Message), true));
    revia::goals::GoalStep submit;
    submit.action.type = revia::actions::ActionType::InvokeControl;
    submit.action.control = "Send";
    const auto decision = gate.Apply(submit, Screen({Field("c1", "Compose")}));
    Check(!decision.allowed,
        "Historical placement authorized submission without any current draft observation.");
}

void TestSubmissionChecksTheLiveDraftAgainAtCommit()
{
    PayloadVault vault;
    auto live = Draft("");
    ContentGate gate(vault, [&](const auto&) { return live; });
    TaskContent task = Exact(Message, "compose");
    task.submissionRequested = true;
    task.submissionVerb = "send";
    gate.BeginTask(task);
    auto entry = LegacyEntry("c1", Message);
    const auto context = Screen({Field("c1", "Compose")});
    Check(gate.Apply(entry, context).allowed, "Initial placement was refused.");
    live.value = Message;
    const auto goal = GoalWith(entry, true);
    gate.ObserveGoal(goal);
    Check(gate.SubmissionReady(), "A freshly verified draft did not become eligible.");
    Check(!gate.CompletionAllowed(), "A send task completed before it submitted anything.");
    revia::goals::GoalStep submit;
    submit.action.type = revia::actions::ActionType::InvokeControl;
    submit.action.control = "Send";
    Check(gate.Apply(submit, context).allowed && submit.action.beforeCommit,
        "Submission did not carry a final runtime draft check.");
    Check(submit.action.beforeCommit().empty(), "The unchanged exact draft was refused.");
    live.value = "changed by the application";
    Check(!submit.action.beforeCommit().empty(), "A draft changed after planning could be submitted.");
    gate.ObserveGoal(goal);
    Check(!gate.Placed(), "Historical verification remained latched after the draft changed.");
    live.value = Message;
    live.context = "recipient=bob";
    Check(!submit.action.beforeCommit().empty(), "An unchanged body with a changed recipient was accepted.");
    live.context = "recipient=alice";
    live.control.runtimeId = "replacement-field";
    Check(!submit.action.beforeCommit().empty(), "A replacement draft control was accepted.");
    live = Draft(Message);
    live.valid = false;
    Check(!submit.action.beforeCommit().empty(), "An unreadable draft was accepted.");
}

void TestOrdinaryNavigationDoesNotRequireADraft()
{
    PayloadVault vault;
    auto live = Draft(Message);
    ContentGate gate(vault, [&](const auto&) { return live; });
    gate.BeginTask(Exact(Message, "compose"));
    ObservedCandidate compose;
    compose.id = "open-compose";
    compose.name = "Compose";
    compose.role = "button";
    compose.mayInvoke = true;
    auto context = Screen({compose});
    revia::goals::GoalStep navigate;
    navigate.action.type = revia::actions::ActionType::InvokeControl;
    navigate.action.control = compose.id;
    Check(gate.Apply(navigate, context).allowed,
        "Opening the editor before placement incorrectly required a submitted draft.");
    context.observation.candidates.front().name = "Alice";
    context.observation.candidates.front().role = "listitem";
    Check(gate.Apply(navigate, context).allowed,
        "Selecting an observed recipient before placement was treated as submitting.");
    context.observation.candidates.front().name = "Inbox";
    context.observation.candidates.front().role = "button";
    Check(gate.Apply(navigate, context).allowed,
        "Opening an observed conversation view before placement was treated as submitting.");
    gate.ObserveGoal(GoalWith(LegacyEntry("c1", Message), true));
    Check(gate.Apply(navigate, context).allowed,
        "Ordinary navigation after draft-only placement was incorrectly treated as sending.");
    auto current = Draft(Message).control;
    current.controlName = "Inbox";
    current.controlType = 50000;
    Check(revia::actions::windows::ValidateActionTarget(navigate.action, current).empty(),
        "The freshly observed ordinary navigation target was refused.");
    current.controlName = "Send";
    Check(!revia::actions::windows::ValidateActionTarget(navigate.action, current).empty(),
        "An Inbox-to-Send change between planning and execution kept the navigation exemption.");
    context.observation.candidates.front().name = "Send";
    Check(!gate.Apply(navigate, context).allowed,
        "Navigation's exception admitted a control now classified as sending.");
    context.observation.candidates.front().name = "Confirm";
    Check(!gate.Apply(navigate, context).allowed,
        "An ambiguous confirmation was exempted as ordinary navigation.");
}

void TestVerifiedSubmissionCanCompleteAfterTheDraftClears()
{
    PayloadVault vault;
    auto live = Draft("");
    ContentGate gate(vault, [&](const auto&) { return live; });
    TaskContent task = Exact(Message, "compose");
    task.submissionRequested = true;
    gate.BeginTask(task);
    auto entry = LegacyEntry("c1", Message);
    const auto context = Screen({Field("c1", "Compose")});
    Check(gate.Apply(entry, context).allowed, "Initial placement was refused.");
    live.value = Message;
    auto goal = GoalWith(entry, true);
    gate.ObserveGoal(goal);
    revia::goals::GoalStep submit;
    submit.action.type = revia::actions::ActionType::InvokeControl;
    submit.action.control = "Send";
    Check(gate.Apply(submit, context).allowed, "Current submission was refused.");
    // GoalRunner marks a refused executor call as attempted/executed. No input receipt
    // means the refusal must remain retryable after the person restores the draft.
    auto refusedGoal = goal;
    refusedGoal.steps.push_back(GoalWith(submit, false).steps.front());
    gate.ObserveGoal(refusedGoal);
    Check(!gate.SubmissionExecuted() && gate.Apply(submit, context).allowed,
        "A known pre-injection refusal was mistaken for an already sent message.");
    auto saved = submit;
    saved.action.onCommitStarted("Save");
    auto savedGoal = goal;
    savedGoal.steps.push_back(GoalWith(saved, true).steps.front());
    gate.ObserveGoal(savedGoal);
    Check(!gate.SubmissionExecuted() && !gate.CompletionAllowed(),
        "A verified Save action was mistaken for submitting the requested message.");
    auto paste = submit;
    paste.action.type = revia::actions::ActionType::PressKeys;
    paste.action.input.keys = "ctrl+v";
    Check(gate.Apply(paste, context).allowed, "A current draft could not receive a guarded edit chord.");
    paste.action.onCommitStarted("Send a message");
    auto pastedGoal = goal;
    pastedGoal.steps.push_back(GoalWith(paste, true).steps.front());
    gate.ObserveGoal(pastedGoal);
    Check(!gate.SubmissionExecuted() && !gate.CompletionAllowed(),
        "Pasting into an edit named Send a message was mistaken for submitting it.");
    Check(gate.Apply(submit, context).allowed, "The actual Send action was refused after Save.");
    submit.action.onCommitStarted("Send");
    submit = GoalWith(submit, true).steps.front();
    goal.steps.push_back(submit);
    live.value.clear();
    gate.ObserveGoal(goal);
    Check(gate.CompletionAllowed(), "A verified send that cleared its draft could not complete.");
    Check(!gate.Apply(submit, context).allowed,
        "A completed submission became eligible to send a second time.");
}

// ---- one gate, every provider ----

// The routine policy leaves `value` empty and lets the controller redeem it; the legacy
// path writes whatever a model produced. Both end here, and both must end at the same
// string -- which is the whole reason the gate sits downstream of the choice of
// provider rather than inside one of them.
void TestEveryProviderShapeEndsAtTheSameValue()
{
    ComputerTaskContext context = Screen({Field("c1", "Compose")});

    PayloadVault routineVault;
    ContentGate routineGate(routineVault);
    routineGate.BeginTask(Exact(Message, "compose"));
    revia::goals::GoalStep routineStep = LegacyEntry("c1", "");
    static_cast<void>(routineGate.Apply(routineStep, context));

    PayloadVault legacyVault;
    ContentGate legacyGate(legacyVault);
    legacyGate.BeginTask(Exact(Message, "compose"));
    revia::goals::GoalStep legacyStep = LegacyEntry("c1", "whatever the model said");
    static_cast<void>(legacyGate.Apply(legacyStep, context));

    Check(routineStep.action.value == legacyStep.action.value,
        "Two providers proposing the same step produced different text: '" +
            routineStep.action.value + "' and '" + legacyStep.action.value + "'.");
    Check(routineStep.action.value == Message,
        "Neither provider ended at the user's own words.");
    Check(routineGate.Stats().inventions == 0 && legacyGate.Stats().inventions == 1,
        "The two shapes were not told apart in the record. A provider that proposed "
        "nothing of its own and one that proposed a sentence are different events.");
}

} // namespace

void RunContentGateTests()
{
    TestTheUsersOwnWordsAreLiftedFromTheRequest();
    TestTheDestinationIsReadAfterTheContentAndNotInsideIt();
    TestAnApostropheIsNotAQuotedSpan();
    TestCurlyQuotesAreRecognised();
    TestADraftingRequestIsContentToComposeRatherThanContentToPlace();
    TestARequestWithNoIdentifiableContentIsLeftAlone();
    TestAPlannersInventedTextNeverReachesTheField();
    TestTheStepDescriptionNamesTheFieldRatherThanTheContent();
    TestAnAlteredCopyOfTheContentIsReplacedAndCounted();
    TestContentIsRefusedWhenTheFieldIsNotTheOneAsked();
    TestAnUnlistedControlIsNotTreatedAsTheWrongField();
    TestAMissingPayloadAsksRatherThanInventing();
    TestThePlaceholderTokenIsNeverTyped();
    TestADraftIsFixedOnceItIsChosen();
    TestCompletionIsRefusedUntilTheContentIsSeenInTheField();
    TestEnteringIncorrectTextIsNotAPlacement();
    TestATaskWithNothingToPlaceFinishesAsItAlwaysDid();
    TestEveryProviderShapeEndsAtTheSameValue();
    TestHistoricalPlacementCannotAuthorizeSubmission();
    TestSubmissionChecksTheLiveDraftAgainAtCommit();
    TestOrdinaryNavigationDoesNotRequireADraft();
    TestVerifiedSubmissionCanCompleteAfterTheDraftClears();

    std::cout << "Exact content is lifted from the request before anything is planned, a planner's "
                 "own words\nnever reach a field, the wrong field is refused, a draft is fixed when "
                 "it is chosen, and no\ntask finishes on content nobody has seen in the box.\n";
}
