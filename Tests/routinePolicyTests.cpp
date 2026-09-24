#include "testSupport.h"

#include "Computer/computerController.h"
#include "Computer/payloadVault.h"
#include "Computer/routinePolicy.h"
#include "Computer/subgoalValidator.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// The decisions that do not need a model, exercised without one.
//
// Nothing here touches a desktop, a backend or a session. That is the point of the
// design and not a convenience of the test: a deterministic policy whose answers depend
// on a machine could not be compared against a model's answers on identical inputs,
// which is the whole basis on which one is allowed to replace the other.

revia::actions::CapabilitySettings ApprovedScope()
{
    revia::actions::CapabilitySettings scope;
    scope.approvedApplications = {"notepad.exe"};
    scope.desktopControl.applicationLaunch = true;
    scope.desktopControl.keyboard = true;
    scope.desktopControl.pointer = true;
    scope.desktopControl.maxTypedCharacters = 512;
    return scope;
}

ComputerObservation Screen(
    const std::string& application,
    std::vector<ObservedCandidate> candidates,
    const std::string& title = "Untitled - Notepad")
{
    ComputerObservation observation;
    observation.screen.succeeded = true;
    observation.screen.generation = 7;
    observation.screen.id = "observation-7";
    observation.screen.foregroundApplication = application;
    observation.screen.foregroundTitle = title;
    observation.candidates = std::move(candidates);
    return observation;
}

ObservedCandidate Button(const std::string& name, const std::string& id = {})
{
    ObservedCandidate candidate;
    candidate.id = id.empty() ? name : id;
    candidate.name = name;
    candidate.role = "button";
    candidate.mayInvoke = true;
    return candidate;
}

ObservedCandidate Edit(const std::string& name, const std::string& id = {})
{
    ObservedCandidate candidate;
    candidate.id = id.empty() ? name : id;
    candidate.name = name;
    candidate.role = "edit";
    candidate.maySetText = true;
    candidate.mayType = true;
    return candidate;
}

ComputerTaskContext Context(ComputerObservation observation)
{
    ComputerTaskContext context;
    context.subgoal = "drive notepad";
    context.actionsLeft = 10;
    context.retriesLeft = 3;
    context.scope = ApprovedScope();
    context.observation = std::move(observation);
    return context;
}

// A validated subgoal, made the way the runtime makes one rather than by setting the
// flag. Going through the validator is the point: a test that stamped `validated` by
// hand would be testing a policy against an input the runtime can never produce.
ComputerSubgoal Validated(
    const SubgoalIntent intent,
    TargetDescriptor target,
    const PayloadVault& vault,
    PayloadReference payload = {})
{
    ComputerSubgoal proposed;
    proposed.id = NewSubgoalId();
    proposed.intent = intent;
    proposed.description = "a bounded step";
    proposed.target = std::move(target);
    proposed.payload = std::move(payload);

    SubgoalContext context;
    context.goalId = "goal-1";
    context.origin = RequestOrigin::UserDirected;
    context.scope = ApprovedScope();
    context.actionsLeft = 10;
    context.retriesLeft = 3;

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(validation.accepted,
        "The test's own subgoal was refused by validation: " + validation.detail);
    return validation.subgoal;
}

TargetDescriptor Target(
    const std::string& application, const std::string& name = {},
    const std::string& role = {})
{
    TargetDescriptor descriptor;
    descriptor.application = application;
    descriptor.name = name;
    descriptor.role = role;
    return descriptor;
}

// ---------------------------------------------------------------------------
// The payload vault
// ---------------------------------------------------------------------------

// The user's exact words go in, a reference comes out, and the reference is the only
// thing a decision ever holds.
void TestAPayloadIsHeldByTheRuntimeAndNotByTheDecision()
{
    PayloadVault vault;
    const PayloadReference reference = vault.Store("Running late, see you at eight", "message");
    Check(reference.Valid() && reference.kind == "message" && reference.length == 30,
        "The reference did not describe the value it stands for.");
    Check(reference.id.find("Running") == std::string::npos &&
            reference.id.find("late") == std::string::npos,
        "The reference leaked the content it exists to stand in for.");
    Check(vault.Redeem(reference).value_or("") == "Running late, see you at eight",
        "The original was not returned intact.");
}

// An invented reference gets nothing at all. Not an empty string: in a field somebody
// is about to submit, an empty string is an empty message actually sent.
void TestAnInventedReferenceRedeemsToNothing()
{
    PayloadVault vault;
    PayloadReference invented;
    invented.id = "payload-made-up";
    invented.length = 12;
    Check(!vault.Holds(invented), "The vault claimed to hold something it never stored.");
    Check(!vault.Redeem(invented).has_value(),
        "An invented payload reference produced a value, which is how a model's "
        "guess becomes a real keystroke.");

    // And a reference that was real stops being real when the task it belonged to ends.
    const PayloadReference real = vault.Store("secret", "message");
    vault.Clear();
    Check(!vault.Redeem(real).has_value(),
        "A payload outlived the task that justified holding it.");
}

// ---------------------------------------------------------------------------
// Subgoal validation
// ---------------------------------------------------------------------------

// The boundary a proposal most plausibly tries to step over, checked against what the
// user authorized rather than against what happens to be on screen.
void TestASubgoalCannotNameAnUnapprovedApplication()
{
    PayloadVault vault;
    ComputerSubgoal proposed;
    proposed.intent = SubgoalIntent::InteractWithControl;
    proposed.target = Target("mimikatz.exe", "Run");

    SubgoalContext context;
    context.goalId = "goal-1";
    context.origin = RequestOrigin::UserDirected;
    context.scope = ApprovedScope();

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(!validation.accepted && validation.rejection == SubgoalRejection::OutsideScope,
        "A subgoal named an application outside the task's scope and was accepted.");
}

// Origin, scope and budget are facts about the run. A proposal that could carry them
// would be a proposal that could change them.
void TestTheRuntimeStampsAuthorityRatherThanReadingIt()
{
    PayloadVault vault;
    ComputerSubgoal proposed;
    proposed.intent = SubgoalIntent::FocusWindow;
    proposed.target = Target("notepad.exe");
    // What a model would write if it could: its own origin, its own scope, its own
    // budget. A claim to have been validated already is deliberately absent, because
    // SubgoalAuthority cannot be granted from out here -- writing that line does not
    // compile, which is a stronger guarantee than a test asserting it was ignored.
    proposed.origin = RequestOrigin::UserDirected;
    proposed.scope.approvedApplications = {"notepad.exe", "cmd.exe"};
    proposed.scope.desktopControl.allowCommandSurfaces = true;
    proposed.actionsLeft = 9999;

    SubgoalContext context;
    context.goalId = "goal-1";
    context.origin = RequestOrigin::Autonomous;
    context.scope = ApprovedScope();
    context.actionsLeft = 4;

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(validation.accepted, "A well-formed subgoal was refused.");
    Check(validation.subgoal.origin == RequestOrigin::Autonomous,
        "A proposal relabelled its own origin, which is how idle work acquires the "
        "permissions of something the user asked for.");
    Check(validation.subgoal.actionsLeft == 4,
        "A proposal wrote its own budget.");
    Check(validation.subgoal.scope.approvedApplications.size() == 1 &&
            !validation.subgoal.scope.desktopControl.allowCommandSurfaces,
        "A proposal widened its own scope.");
}

// A proposal naming content the runtime is not holding gets refused, not an empty value.
void TestASubgoalCannotReferenceContentTheRuntimeDoesNotHold()
{
    PayloadVault vault;
    ComputerSubgoal proposed;
    proposed.intent = SubgoalIntent::EnterPayload;
    proposed.target = Target("notepad.exe", "Text editor");
    proposed.payload.id = "payload-invented";
    proposed.payload.length = 8;

    SubgoalContext context;
    context.goalId = "goal-1";
    context.scope = ApprovedScope();

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(!validation.accepted && validation.rejection == SubgoalRejection::UnknownPayload,
        "A subgoal referenced content that does not exist and was accepted.");
}

// A subgoal may not choose a convenient thing to be graded on.
void TestASubgoalCannotBeGradedOnSomethingElse()
{
    PayloadVault vault;
    ComputerSubgoal proposed;
    proposed.intent = SubgoalIntent::FocusWindow;
    proposed.target = Target("notepad.exe");
    SubgoalPostcondition elsewhere;
    elsewhere.kind = revia::goals::PostconditionKind::ForegroundApplicationIs;
    // Not the application this subgoal is about.
    elsewhere.value = "explorer.exe";
    proposed.postconditions.push_back(elsewhere);

    SubgoalContext context;
    context.goalId = "goal-1";
    context.scope = ApprovedScope();

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(!validation.accepted &&
            validation.rejection == SubgoalRejection::UnverifiablePostcondition,
        "A subgoal asked to be graded on something other than what it was asked to do.");
}

// A contract this build does not implement is refused by name rather than partly read.
void TestAnUnknownSchemaIsRefused()
{
    PayloadVault vault;
    ComputerSubgoal proposed;
    proposed.schemaVersion = CurrentSubgoalSchema + 1;
    proposed.intent = SubgoalIntent::FocusWindow;
    proposed.target = Target("notepad.exe");

    SubgoalContext context;
    context.scope = ApprovedScope();
    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    Check(!validation.accepted &&
            validation.rejection == SubgoalRejection::UnsupportedSchema,
        "A subgoal from a contract this build does not implement was interpreted anyway.");
}

// ---------------------------------------------------------------------------
// The routine policy
// ---------------------------------------------------------------------------

// The decision the whole feature is for: the subgoal says which window, the observation
// says it is not in front, and what to do next follows from those two facts.
void TestARoutineFocusNeedsNoModel()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(Validated(SubgoalIntent::FocusWindow, Target("notepad.exe"), vault));

    const ComputerDecision decision = policy.Decide(
        Context(Screen("explorer.exe", {})), {});
    Check(decision.kind == ComputerDecisionKind::ProposeAction &&
            decision.step.action.type == revia::actions::ActionType::FocusWindow &&
            decision.step.action.application == "notepad.exe",
        "A routine focus was not proposed from an observation that plainly called for it.");
    Check(decision.costReported && decision.tokens == 0,
        "A decision that reached no backend did not report costing nothing, which is "
        "the measurement this feature exists to produce.");
}

// Already done is a completion proposal, not a redundant action.
void TestAlreadySatisfiedProposesCompletion()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(Validated(SubgoalIntent::FocusWindow, Target("notepad.exe"), vault));

    const ComputerDecision decision = policy.Decide(
        Context(Screen("notepad.exe", {})), {});
    Check(decision.kind == ComputerDecisionKind::ProposeCompletion,
        "A subgoal that was already satisfied proposed an action anyway.");
}

// One match is a decision. Two are a question, and the question goes to a person.
void TestAnAmbiguousTargetEscalatesRatherThanGuessing()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(
        Validated(SubgoalIntent::InteractWithControl, Target("notepad.exe", "Send"), vault));

    const ComputerDecision one = policy.Decide(
        Context(Screen("notepad.exe", {Button("Send", "send-1"), Button("Cancel")})), {});
    Check(one.kind == ComputerDecisionKind::ProposeAction &&
            one.step.action.control == "send-1",
        "An unambiguous target was not acted on.");

    const ComputerDecision two = policy.Decide(
        Context(Screen("notepad.exe", {Button("Send", "send-1"), Button("Send", "send-2")})),
        {});
    Check(two.kind == ComputerDecisionKind::NeedUser,
        "Two controls matched one description and the policy picked one, which is how "
        "a message goes to the wrong recipient.");
}

// A name is matched whole. "Send" must not press "Send later".
void TestANameIsMatchedWholeRatherThanAsAFragment()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(
        Validated(SubgoalIntent::InteractWithControl, Target("notepad.exe", "Send"), vault));

    const ComputerDecision decision = policy.Decide(
        Context(Screen("notepad.exe", {Button("Send later"), Button("Send anyway")})), {});
    Check(decision.kind != ComputerDecisionKind::ProposeAction,
        "A descriptor matched a longer name it does not name, and proposed pressing it.");
}

// A capped listing that did not contain the target is not evidence the target is absent.
void TestATruncatedListingAsksForAnotherLookRatherThanConcluding()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(
        Validated(SubgoalIntent::InteractWithControl, Target("notepad.exe", "Send"), vault));

    ComputerObservation truncated = Screen("notepad.exe", {Button("Cancel")});
    truncated.screen.omittedControls = 120;
    const ComputerDecision decision = policy.Decide(Context(truncated), {});
    Check(decision.kind == ComputerDecisionKind::Reobserve,
        "A bounded listing was treated as a complete statement about what is on screen.");

    ComputerObservation complete = Screen("notepad.exe", {Button("Cancel")});
    const ComputerDecision absent = policy.Decide(Context(complete), {});
    Check(absent.kind == ComputerDecisionKind::NeedReasoning,
        "A target genuinely absent from a complete listing did not escalate.");
}

// A blind moment is not a reason to act anyway, and the two kinds of blindness want
// different answers.
void TestBlindnessIsNotActedThrough()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(
        Validated(SubgoalIntent::InteractWithControl, Target("notepad.exe", "Send"), vault));

    ComputerObservation failed;
    failed.screen.succeeded = false;
    failed.screen.failure = "the desktop could not be read";
    Check(policy.Decide(Context(failed), {}).kind == ComputerDecisionKind::Reobserve,
        "A failed observation did not ask for another look.");

    ComputerObservation withheld = Screen("keepass.exe", {});
    withheld.withheld = true;
    Check(policy.Decide(Context(withheld), {}).kind == ComputerDecisionKind::NeedVision,
        "A window withheld by the perception filter was treated as a look that might "
        "succeed next time, which loops rather than escalating.");
}

// A disambiguator the observation cannot confirm is escalated, never ignored. Ignoring
// it would widen the match to every same-named control in the window.
void TestAnUnconfirmableContainerEscalates()
{
    PayloadVault vault;
    TargetDescriptor target = Target("notepad.exe", "Send");
    target.container = "the compose pane";
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(Validated(SubgoalIntent::InteractWithControl, target, vault));

    const ComputerDecision decision = policy.Decide(
        Context(Screen("notepad.exe", {Button("Send")})), {});
    Check(decision.kind == ComputerDecisionKind::NeedReasoning,
        "A container the observation cannot confirm was quietly dropped from the match.");
}

// The target lives in a particular window, and acting into the wrong one is the failure
// this check exists to prevent.
void TestTheWrongWindowIsFocusedRatherThanTypedInto()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(
        Validated(SubgoalIntent::InteractWithControl, Target("notepad.exe", "Send"), vault));

    const ComputerDecision decision = policy.Decide(
        Context(Screen("explorer.exe", {Button("Send")})), {});
    Check(decision.kind == ComputerDecisionKind::ProposeAction &&
            decision.step.action.type == revia::actions::ActionType::FocusWindow,
        "A control was pressed in whatever window happened to contain a matching name.");
}

// The policy proposes where the content goes. It never holds the content.
void TestAPayloadStepCarriesAReferenceAndNotTheWords()
{
    PayloadVault vault;
    const PayloadReference reference =
        vault.Store("Running late, see you at eight", "message");
    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(Validated(
        SubgoalIntent::EnterPayload, Target("notepad.exe", "Text editor"), vault, reference));

    const ComputerDecision decision = policy.Decide(
        Context(Screen("notepad.exe", {Edit("Text editor", "edit-15")})), {});
    Check(decision.kind == ComputerDecisionKind::ProposeAction &&
            decision.step.action.type == revia::actions::ActionType::SetControlText &&
            decision.step.action.control == "edit-15",
        "A payload entry was not proposed against the field that can hold it.");
    Check(decision.step.action.value.empty() && decision.payload.id == reference.id,
        "A policy wrote the user's words into the step instead of naming the slot.");
    // And the words do not reach the record on their way to being verified.
    Check(decision.step.expected.find("Running late") == std::string::npos,
        "The user's own words reached the goal record through the expected text.");
}

// A policy with no bounded subgoal is deterministic about nothing, and says so.
void TestNoSubgoalMeansNoRoutineDecision()
{
    PayloadVault vault;
    RoutineComputerPolicy policy(vault);
    const ComputerDecision decision = policy.Decide(
        Context(Screen("notepad.exe", {Button("Send")})), {});
    Check(decision.kind == ComputerDecisionKind::CannotHandle &&
            decision.code == ComputerReasonCode::OutsideQualifiedScope,
        "A routine policy invented a goal from the free text it was not given.");
}

// ---------------------------------------------------------------------------
// Provider modes and routing
// ---------------------------------------------------------------------------

// A policy that answers whatever it is told to, for driving the controller.
class ScriptedProvider final : public IComputerPolicy
{
public:
    ScriptedProvider(std::string providerName, ComputerDecision answer)
        : name(std::move(providerName)), scripted(std::move(answer))
    {
    }

    [[nodiscard]] std::string Name() const override { return name; }
    [[nodiscard]] bool IsAvailable() const override { return available; }

    [[nodiscard]] ComputerDecision Decide(
        const ComputerTaskContext&, std::stop_token) override
    {
        ++calls;
        ComputerDecision answer = scripted;
        answer.provider = name;
        return answer;
    }

    std::string name;
    ComputerDecision scripted;
    bool available = true;
    int calls = 0;
};

ComputerDecision LegacyProposes()
{
    ComputerDecision answer;
    answer.kind = ComputerDecisionKind::ProposeAction;
    answer.step.description = "Press Send";
    answer.step.action.type = revia::actions::ActionType::InvokeControl;
    answer.step.action.application = "notepad.exe";
    answer.step.action.control = "Send";
    answer.step.check.type = revia::actions::ActionType::InspectWindow;
    answer.step.check.application = "notepad.exe";
    answer.step.expected = "Send was used";
    answer.tokens = 420;
    answer.costReported = true;
    return answer;
}

// The default changes nothing. Installing the feature must not move a single decision.
void TestLegacyModeAsksOnlyTheExistingPath()
{
    PayloadVault vault;
    ComputerController controller{vault};
    auto legacy = std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes());
    ScriptedProvider& watchedLegacy = *legacy;
    controller.SetLegacyPolicy(std::move(legacy));
    auto routine = std::make_unique<RoutineComputerPolicy>(vault);
    routine->SetSubgoal(Validated(SubgoalIntent::FocusWindow, Target("notepad.exe"), vault));
    controller.SetRoutinePolicy(std::move(routine));

    Check(controller.Mode() == ComputerProviderMode::Legacy,
        "The controller did not default to the existing decision path.");
    const auto next = controller.Decide(Context(Screen("explorer.exe", {})), {});
    Check(next.hasStep && watchedLegacy.calls == 1,
        "The existing path was not the one asked in the default mode.");
    Check(controller.Stats().modelCalls == 1 && controller.Stats().routineDecisions == 0,
        "A decision was routed away from the model in a mode that must not route "
        "anything.");
}

// The mode that actually saves the call.
void TestAssistedModeKeepsARoutineDecisionAwayFromTheModel()
{
    PayloadVault vault;
    ComputerController controller{vault};
    auto legacy = std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes());
    ScriptedProvider& watchedLegacy = *legacy;
    controller.SetLegacyPolicy(std::move(legacy));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    Check(controller.SetSubgoal(
            Validated(SubgoalIntent::FocusWindow, Target("notepad.exe"), vault)),
        "A validated subgoal was refused by the controller.");
    controller.SetMode(ComputerProviderMode::Assisted);

    const auto next = controller.Decide(Context(Screen("explorer.exe", {})), {});
    Check(next.hasStep &&
            next.step.action.type == revia::actions::ActionType::FocusWindow,
        "Assisted mode did not act on the routine decision.");
    Check(watchedLegacy.calls == 0,
        "A decision a deterministic policy had already answered was sent to the model "
        "anyway, which is the cost this feature exists to remove.");
    Check(controller.Stats().routineDecisions == 1 &&
            controller.Stats().modelCalls == 0 &&
            controller.Stats().tokens == 0,
        "The saving was not recorded, so it cannot be reported.");
}

// And when the cheap policy has nothing, the expensive one still answers.
void TestAssistedModeFallsBackRatherThanGuessing()
{
    PayloadVault vault;
    ComputerController controller{vault};
    auto legacy = std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes());
    ScriptedProvider& watchedLegacy = *legacy;
    controller.SetLegacyPolicy(std::move(legacy));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    // A subgoal whose container the observation cannot confirm: the routine policy
    // abstains by design.
    TargetDescriptor target = Target("notepad.exe", "Send");
    target.container = "the compose pane";
    Check(controller.SetSubgoal(
            Validated(SubgoalIntent::InteractWithControl, target, vault)),
        "A validated subgoal was refused by the controller.");
    controller.SetMode(ComputerProviderMode::Assisted);

    const auto next = controller.Decide(
        Context(Screen("notepad.exe", {Button("Send")})), {});
    Check(next.hasStep && watchedLegacy.calls == 1,
        "A decision the routine policy abstained on did not reach the model.");
    Check(controller.Stats().escalations == 1 && controller.Stats().modelCalls == 1,
        "The escalation was not counted, so the cost of abstaining is invisible.");
    Check(controller.LastDecision().escalated &&
            controller.LastDecision().escalatedFrom == "routine",
        "The record does not say which provider handed the decision on.");
}

// A run that keeps escalating stops paying for the attempt.
void TestEscalationIsBounded()
{
    PayloadVault vault;
    ComputerController controller{vault};
    controller.SetLegacyPolicy(
        std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes()));
    auto routine = std::make_unique<RoutineComputerPolicy>(vault);
    RoutineComputerPolicy& watchedRoutine = *routine;
    controller.SetRoutinePolicy(std::move(routine));
    TargetDescriptor target = Target("notepad.exe", "Send");
    target.container = "the compose pane";
    Check(controller.SetSubgoal(
            Validated(SubgoalIntent::InteractWithControl, target, vault)),
        "A validated subgoal was refused by the controller.");
    controller.SetMode(ComputerProviderMode::Assisted);
    controller.SetEscalationBudget(2);

    for (int iteration = 0; iteration < 5; ++iteration)
    {
        static_cast<void>(controller.Decide(
            Context(Screen("notepad.exe", {Button("Send")})), {}));
    }
    Check(controller.Stats().escalations == 2,
        "A run kept paying for a routine attempt that had already proved it could not "
        "help; escalations were " + std::to_string(controller.Stats().escalations) + ".");
    static_cast<void>(watchedRoutine);
}

// Shadow mode measures without touching anything.
void TestShadowModeComparesWithoutExecuting()
{
    PayloadVault vault;
    ComputerController controller{vault};
    auto legacy = std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes());
    ScriptedProvider& watchedLegacy = *legacy;
    controller.SetLegacyPolicy(std::move(legacy));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    Check(controller.SetSubgoal(
            Validated(SubgoalIntent::InteractWithControl, Target("notepad.exe", "Send"),
                vault)),
        "A validated subgoal was refused by the controller.");
    controller.SetMode(ComputerProviderMode::Shadow);

    const auto next = controller.Decide(
        Context(Screen("notepad.exe", {Button("Send")})), {});
    Check(next.hasStep && watchedLegacy.calls == 1,
        "Shadow mode did not let the existing path decide and execute.");
    Check(next.step.action.control == "Send",
        "The shadow's proposal reached the runner, which is the one thing shadow mode "
        "must never do.");
    Check(controller.LastDecision().shadowEvaluated &&
            controller.LastDecision().shadowProvider == "routine",
        "No comparison was recorded, so shadow mode measured nothing.");
    Check(controller.LastDecision().shadowAgreed &&
            controller.Stats().shadowAgreements == 1,
        "Two providers proposing the same action on the same control were not counted "
        "as agreeing.");
    Check(controller.Stats().modelCalls == 1,
        "Shadow mode changed how many decisions reached the model.");
}

// Disagreement is recorded as disagreement rather than quietly preferred either way.
void TestShadowDisagreementIsRecorded()
{
    PayloadVault vault;
    ComputerController controller{vault};
    controller.SetLegacyPolicy(
        std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes()));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    // The routine policy will propose focusing, not pressing.
    Check(controller.SetSubgoal(
            Validated(SubgoalIntent::FocusWindow, Target("notepad.exe"), vault)),
        "A validated subgoal was refused by the controller.");
    controller.SetMode(ComputerProviderMode::Shadow);

    static_cast<void>(controller.Decide(Context(Screen("explorer.exe", {})), {}));
    Check(controller.LastDecision().shadowEvaluated &&
            !controller.LastDecision().shadowAgreed &&
            controller.Stats().shadowAgreements == 0,
        "Two providers proposing different actions were recorded as agreeing.");
}

// An unqualified learned artifact leaves the mode inactive rather than quietly
// behaving like something else.
void TestLearnedModeWithoutAnArtifactIsInactiveAndSaysWhy()
{
    PayloadVault vault;
    ComputerController controller{vault};
    controller.SetLegacyPolicy(
        std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes()));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    Check(controller.SetSubgoal(
            Validated(SubgoalIntent::FocusWindow, Target("notepad.exe"), vault)),
        "A validated subgoal was refused by the controller.");

    auto unloaded = std::make_unique<ScriptedProvider>("learned", ComputerDecision{});
    unloaded->available = false;
    ScriptedProvider& watched = *unloaded;
    controller.SetLearnedPolicy(std::move(unloaded));
    controller.SetMode(ComputerProviderMode::Learned);

    Check(controller.EffectiveMode() == ComputerProviderMode::Assisted,
        "An unavailable learned artifact did not fall back to the policy that works.");
    Check(!controller.ModeUnavailableReason().empty(),
        "The reason a selected mode is inactive was not reported, leaving a user unable "
        "to tell a missing artifact from a refused permission.");
    static_cast<void>(controller.Decide(Context(Screen("explorer.exe", {})), {}));
    Check(watched.calls == 0,
        "An unavailable learned policy was asked anyway.");
}

// Assisted mode with nothing to work from is the existing path, not a guess.
void TestAssistedWithoutASubgoalIsTheExistingPath()
{
    PayloadVault vault;
    ComputerController controller{vault};
    auto legacy = std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes());
    ScriptedProvider& watchedLegacy = *legacy;
    controller.SetLegacyPolicy(std::move(legacy));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    controller.SetMode(ComputerProviderMode::Assisted);

    Check(controller.EffectiveMode() == ComputerProviderMode::Legacy,
        "Assisted mode stayed selected with no bounded subgoal to work from.");
    static_cast<void>(controller.Decide(Context(Screen("explorer.exe", {})), {}));
    Check(watchedLegacy.calls == 1,
        "A task with no subgoal did not fall back to the existing decision path.");
}

// A subgoal that has not been through validation is a model's opinion, and the seam
// that enforces that is here.
void TestAnUnvalidatedSubgoalIsRefusedByTheController()
{
    PayloadVault vault;
    ComputerController controller{vault};
    controller.SetLegacyPolicy(
        std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes()));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));

    ComputerSubgoal unchecked;
    unchecked.intent = SubgoalIntent::InteractWithControl;
    unchecked.target = Target("anything.exe", "Delete everything");
    unchecked.schemaVersion = CurrentSubgoalSchema;
    // There is deliberately no line here setting the stamp. That is the assertion: the
    // authority a model would grant itself is not reachable from outside the validator,
    // so this cannot be written even to test it.
    Check(!unchecked.Validated(),
        "A subgoal assembled by hand claimed to have been validated.");

    Check(!controller.SetSubgoal(unchecked),
        "The controller accepted a subgoal that never went through validation.");
    controller.SetMode(ComputerProviderMode::Assisted);
    const auto next = controller.Decide(
        Context(Screen("anything.exe", {Button("Delete everything")})), {});
    Check(next.step.action.application != "anything.exe",
        "A subgoal naming an application outside the task's scope reached a policy "
        "and produced a step for it.");

    // And the same proposal, put through validation, is refused there for the reason it
    // should be refused: the application is not in this task's scope.
    SubgoalContext scoped;
    scoped.goalId = "goal-1";
    scoped.scope = ApprovedScope();
    const SubgoalValidation validation = ValidateSubgoal(unchecked, scoped, vault);
    Check(!validation.accepted && validation.rejection == SubgoalRejection::OutsideScope,
        "Validation did not refuse an out-of-scope application.");
}

// A reference the vault no longer holds refuses the step. It never becomes an empty
// value typed into a field somebody is about to submit.
void TestAnUnredeemablePayloadRefusesTheStep()
{
    PayloadVault vault;
    ComputerController controller{vault};
    ComputerDecision proposing = LegacyProposes();
    proposing.step.action.type = revia::actions::ActionType::SetControlText;
    proposing.step.action.control = "edit-15";
    proposing.payload.id = "payload-gone";
    proposing.payload.length = 30;
    controller.SetLegacyPolicy(
        std::make_unique<ScriptedProvider>("legacy_llm", std::move(proposing)));

    const auto next = controller.Decide(
        Context(Screen("notepad.exe", {Edit("Text editor", "edit-15")})), {});
    Check(!next.hasStep,
        "A step whose content could not be found was dispatched anyway, which in a "
        "field about to be submitted is an empty message actually sent.");
    Check(controller.Stats().payloadRefusals == 1,
        "The refusal was not counted.");
}

// And the ordinary case: the controller fills it, not the policy.
void TestTheControllerSuppliesTheWordsAfterTheDecision()
{
    PayloadVault vault;
    const PayloadReference reference =
        vault.Store("Running late, see you at eight", "message");
    ComputerController controller{vault};
    controller.SetLegacyPolicy(
        std::make_unique<ScriptedProvider>("legacy_llm", LegacyProposes()));
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    Check(controller.SetSubgoal(Validated(SubgoalIntent::EnterPayload,
            Target("notepad.exe", "Text editor"), vault, reference)),
        "A validated payload subgoal was refused by the controller.");
    controller.SetMode(ComputerProviderMode::Assisted);

    const auto next = controller.Decide(
        Context(Screen("notepad.exe", {Edit("Text editor", "edit-15")})), {});
    Check(next.hasStep &&
            next.step.action.value == "Running late, see you at eight",
        "The runtime did not supply the original content after the decision was made.");
    Check(controller.Stats().routineDecisions == 1 && controller.Stats().modelCalls == 0,
        "A payload entry that needed no reasoning was sent to the model.");
}

} // namespace

void RunRoutinePolicyTests()
{
    TestAPayloadIsHeldByTheRuntimeAndNotByTheDecision();
    TestAnInventedReferenceRedeemsToNothing();

    TestASubgoalCannotNameAnUnapprovedApplication();
    TestTheRuntimeStampsAuthorityRatherThanReadingIt();
    TestASubgoalCannotReferenceContentTheRuntimeDoesNotHold();
    TestASubgoalCannotBeGradedOnSomethingElse();
    TestAnUnknownSchemaIsRefused();

    TestARoutineFocusNeedsNoModel();
    TestAlreadySatisfiedProposesCompletion();
    TestAnAmbiguousTargetEscalatesRatherThanGuessing();
    TestANameIsMatchedWholeRatherThanAsAFragment();
    TestATruncatedListingAsksForAnotherLookRatherThanConcluding();
    TestBlindnessIsNotActedThrough();
    TestAnUnconfirmableContainerEscalates();
    TestTheWrongWindowIsFocusedRatherThanTypedInto();
    TestAPayloadStepCarriesAReferenceAndNotTheWords();
    TestNoSubgoalMeansNoRoutineDecision();

    TestLegacyModeAsksOnlyTheExistingPath();
    TestAssistedModeKeepsARoutineDecisionAwayFromTheModel();
    TestAssistedModeFallsBackRatherThanGuessing();
    TestEscalationIsBounded();
    TestShadowModeComparesWithoutExecuting();
    TestShadowDisagreementIsRecorded();
    TestLearnedModeWithoutAnArtifactIsInactiveAndSaysWhy();
    TestAssistedWithoutASubgoalIsTheExistingPath();
    TestAnUnvalidatedSubgoalIsRefusedByTheController();
    TestAnUnredeemablePayloadRefusesTheStep();
    TestTheControllerSuppliesTheWordsAfterTheDecision();

    std::cout << "Routine decisions are made without a model, abstain rather than guess, "
                 "and never hold the words they place.\n";
}
