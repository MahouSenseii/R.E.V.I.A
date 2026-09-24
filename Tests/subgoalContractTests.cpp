#include "testSupport.h"

#include "Computer/computerController.h"
#include "Computer/payloadVault.h"
#include "Computer/routinePolicy.h"
#include "Computer/subgoalPlanner.h"
#include "Computer/subgoalValidator.h"
#include "Planning/goalPlanner.h"

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// The subgoal contract, inspected rather than trusted.
//
// The defect that motivated every test in this file could not be seen from any counter.
// `SetSubgoalPlanner` was wired to `PlanNextGoalStep`, whose grammar forces a next-step
// object, so Main returned a step to every subgoal prompt and validation refused all of
// them. Assisted mode fell back on every iteration and the run still succeeded, so no
// assertion anywhere noticed. The tests passed because a scripted test supplies the
// answer, and the answer was the only thing that was wrong.
//
// So these look at the request itself: which instruction is sent, which schema
// constrains the reply, and what shape of answer that schema actually admits. A
// contract nothing inspects is a contract that can be rewired without a single test
// going red.

revia::actions::CapabilitySettings ApprovedScope()
{
    revia::actions::CapabilitySettings scope;
    scope.approvedApplications = {"reviadesktopfixture.exe"};
    scope.desktopControl.applicationLaunch = true;
    scope.desktopControl.keyboard = true;
    scope.desktopControl.pointer = true;
    scope.desktopControl.maxTypedCharacters = 512;
    return scope;
}

ObservedCandidate Control(
    const std::string& name, const std::string& role, const bool editable)
{
    ObservedCandidate candidate;
    candidate.id = name.empty() ? "unnamed-1" : name;
    candidate.name = name;
    candidate.role = role;
    candidate.nameless = name.empty();
    candidate.mayInvoke = !editable;
    candidate.maySetText = editable;
    candidate.mayType = editable;
    return candidate;
}

ComputerTaskContext FixtureScreen()
{
    ComputerTaskContext context;
    context.subgoal = "put <the prepared content> in the Compose box";
    context.actionsLeft = 10;
    context.retriesLeft = 3;
    context.scope = ApprovedScope();
    context.observation.screen.succeeded = true;
    context.observation.screen.generation = 3;
    context.observation.screen.foregroundApplication = "reviadesktopfixture.exe";
    context.observation.screen.foregroundTitle = "Revia Fixture";
    context.observation.candidates = {
        Control("Compose", "edit", /*editable=*/true),
        Control("Send", "button", /*editable=*/false),
        Control("Zoom in", "button", /*editable=*/false)};
    return context;
}

// The contract, field by field. Every one of these was true before and none of them was
// checked, which is how the wiring defect survived a full suite.
void TestTheSubgoalRequestAsksTheSubgoalQuestion()
{
    PayloadVault vault;
    const PayloadReference payload = vault.Store("dinner at eight", "message");
    ComputerTaskContext context = FixtureScreen();
    context.preparedContent.held = true;
    context.preparedContent.kind = "message";
    context.preparedContent.length = payload.length;
    context.preparedContent.destination = "compose";

    const SubgoalRequest request =
        FormatSubgoalRequest(context.subgoal, context, payload);

    // The instruction is the system prompt and has to be about subgoals. The next-step
    // prompt is about steps, and sending that one is exactly the defect.
    Check(request.instruction.find("bounded piece of local progress") != std::string::npos,
        "The instruction is not the subgoal instruction.");
    Check(request.instruction.find("enter_payload") != std::string::npos,
        "The instruction does not describe the subgoal vocabulary.");
    Check(request.instruction.find("\"decision\"") == std::string::npos &&
            request.instruction.find("check") == std::string::npos,
        "The instruction still describes the next-step contract, which is the wiring "
        "defect this test exists to catch.");

    // The situation is the user message and carries the screen.
    const nlohmann::json situation = nlohmann::json::parse(request.situation);
    Check(situation.contains("candidates") && situation["candidates"].size() == 3,
        "The situation did not carry what is on screen.");
    Check(situation.contains("available_payload") &&
            situation["available_payload"].is_object(),
        "The situation did not reference the held content.");
    Check(situation["available_payload"].value("id", std::string()) == payload.id,
        "The payload was referenced by something other than its id.");
    Check(request.situation.find("dinner at eight") == std::string::npos,
        "The user's exact words reached the subgoal prompt.");
    Check(situation["available_payload"].value("destination", std::string()) == "compose",
        "The field the user named was not passed to the planner, so it has to guess "
        "where the content goes.");

    // And the schema, which is the half that actually decides the answer's shape.
    const nlohmann::json schema = nlohmann::json::parse(request.schema);
    Check(schema.contains("properties") && schema["properties"].contains("intent"),
        "The schema does not constrain the answer to a subgoal.");
    Check(!schema["properties"].contains("action") &&
            !schema["properties"].contains("step"),
        "The schema still admits a next-step object. A grammar that forces a step is a "
        "grammar that answers for the model, whatever the prompt says.");

    const nlohmann::json intents = schema["properties"]["intent"]["enum"];
    Check(intents.size() == 7,
        "The intent vocabulary is not the one this build implements.");
    Check(std::find(intents.begin(), intents.end(), "enter_payload") != intents.end(),
        "The schema does not admit entering a payload.");

    // Names constrained to what is on screen. A name the model invents is one nothing
    // can match, and the run pays a model call to find that out.
    const nlohmann::json names =
        schema["properties"]["target"]["properties"]["name"]["enum"];
    Check(std::find(names.begin(), names.end(), "Compose") != names.end(),
        "A control that is on screen was not offered to the planner.");
    Check(std::find(names.begin(), names.end(), "Notepad") == names.end(),
        "The schema admitted a name nothing on screen carries.");
    Check(std::find(names.begin(), names.end(), "") != names.end(),
        "The schema refused the empty name, which is the only way to reach an "
        "unlabelled field by its container.");
}

// The two routes are different questions and must stay different. This compares the
// grammars directly, because that is the thing that was shared.
void TestTheSubgoalAndNextStepGrammarsAreNotInterchangeable()
{
    PayloadVault vault;
    const ComputerTaskContext context = FixtureScreen();
    const SubgoalRequest request =
        FormatSubgoalRequest(context.subgoal, context, PayloadReference{});
    const std::string nextStep = revia::planning::GoalPlanner::NextStepSchema("{}");

    Check(request.schema != nextStep,
        "The subgoal and next-step schemas are the same string. Asking the subgoal "
        "question under the step grammar is the defect that made assisted mode "
        "unreachable on a live backend.");
    Check(nextStep.find("\"decision\"") != std::string::npos,
        "The next-step schema stopped requiring a decision, so this comparison no "
        "longer proves anything.");
    Check(request.schema.find("intent") != std::string::npos,
        "The subgoal schema stopped requiring an intent.");
}

// ---- placing is not sending ----

ComputerSubgoal Proposal(const SubgoalIntent intent, const std::string& name)
{
    ComputerSubgoal proposed;
    proposed.schemaVersion = CurrentSubgoalSchema;
    proposed.intent = intent;
    proposed.description = "a bounded step";
    proposed.target.application = "reviadesktopfixture.exe";
    proposed.target.name = name;
    return proposed;
}

SubgoalContext TaskContext(const bool contentPending)
{
    SubgoalContext context;
    context.goalId = "goal-1";
    context.origin = RequestOrigin::UserDirected;
    context.scope = ApprovedScope();
    context.actionsLeft = 8;
    context.retriesLeft = 2;
    context.contentPending = contentPending;
    return context;
}

void TestATaskToPlaceTextDoesNotAuthoriseSend()
{
    PayloadVault vault;
    const SubgoalValidation refused = ValidateSubgoal(
        Proposal(SubgoalIntent::InteractWithControl, "Send"),
        TaskContext(/*contentPending=*/true), vault);

    Check(!refused.accepted,
        "A subgoal proposing Send was accepted while the content it would send had not "
        "been placed. This is what the live model actually proposed.");
    Check(refused.rejection == SubgoalRejection::SendBeforePlacement,
        "The refusal was recorded as something other than what it is, so it cannot be "
        "counted or distinguished from a scope problem.");
}

void TestSendIsStillReachableOnceTheContentIsIn()
{
    PayloadVault vault;
    const SubgoalValidation accepted = ValidateSubgoal(
        Proposal(SubgoalIntent::InteractWithControl, "Send"),
        TaskContext(/*contentPending=*/false), vault);

    Check(accepted.accepted,
        "Pressing Send was refused on a task with nothing pending, which would make "
        "the ordering rule a blanket ban. It is an ordering rule.");
}

void TestOrdinaryControlsAreUnaffectedByTheOrderingRule()
{
    PayloadVault vault;
    const SubgoalValidation accepted = ValidateSubgoal(
        Proposal(SubgoalIntent::InteractWithControl, "Zoom in"),
        TaskContext(/*contentPending=*/true), vault);

    Check(accepted.accepted,
        "An ordinary button was refused because content was pending, which would stop "
        "the preparation steps a task needs before it can place anything.");
}

// Resolving a field finds it. That is all it does, and a task whose point is to put
// something in the field is not finished by having located it.
void TestResolvingAFieldIsNotPlacingContent()
{
    PayloadVault vault;
    const PayloadReference payload = vault.Store("dinner at eight", "message");
    ComputerSubgoal resolve = Proposal(SubgoalIntent::ResolveTarget, "Compose");
    const SubgoalValidation validated =
        ValidateSubgoal(resolve, TaskContext(/*contentPending=*/true), vault);
    Check(validated.accepted, "Resolving a target was refused, which it should not be.");

    RoutineComputerPolicy policy(vault);
    policy.SetSubgoal(validated.subgoal);
    const ComputerDecision decision =
        policy.Decide(FixtureScreen(), std::stop_token{});

    // The subgoal is complete -- it found the thing. What matters is that the decision
    // says so about the subgoal and proposes no action, so nothing downstream can read
    // it as the content having been placed.
    Check(decision.kind == ComputerDecisionKind::ProposeCompletion,
        "Resolving a target did not complete its own subgoal.");
    Check(decision.step.action.type == revia::actions::ActionType::Unknown,
        "Resolving a target proposed an action. It is read-only by construction: the "
        "answer is that the control is there.");
}

// ---- a subgoal is not re-planned because a step happened ----

void TestAValidSubgoalSurvivesTheStepsTakenUnderIt()
{
    PayloadVault vault;
    const PayloadReference payload = vault.Store("dinner at eight", "message");
    ComputerSubgoal proposed = Proposal(SubgoalIntent::EnterPayload, "Compose");
    proposed.payload = payload;
    const SubgoalValidation validated =
        ValidateSubgoal(proposed, TaskContext(/*contentPending=*/true), vault);
    Check(validated.accepted, "The payload subgoal was refused.");

    ComputerController controller(vault);
    controller.SetRoutinePolicy(std::make_unique<RoutineComputerPolicy>(vault));
    controller.SetMode(ComputerProviderMode::Assisted);
    Check(controller.SetSubgoal(validated.subgoal), "The subgoal was not installed.");

    // Several iterations, with the screen changing under it the way it does when steps
    // are actually being taken. The subgoal is a bounded piece of work, not a statement
    // about one screen, and dropping it whenever something happened would buy a fresh
    // model call for every step -- which is the cost this whole arrangement exists to
    // avoid.
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        ComputerTaskContext context = FixtureScreen();
        context.iteration = static_cast<std::uint32_t>(iteration);
        context.observation.screen.generation = 3 + iteration;
        context.observation.changedSinceLastDecision = iteration > 0;
        static_cast<void>(controller.Decide(context, std::stop_token{}));
        Check(controller.HasSubgoal(),
            "The subgoal was dropped after iteration " + std::to_string(iteration) +
                ", so the next one would cost another model call for no new reason.");
    }
}

} // namespace

void RunSubgoalContractTests()
{
    TestTheSubgoalRequestAsksTheSubgoalQuestion();
    TestTheSubgoalAndNextStepGrammarsAreNotInterchangeable();
    TestATaskToPlaceTextDoesNotAuthoriseSend();
    TestSendIsStillReachableOnceTheContentIsIn();
    TestOrdinaryControlsAreUnaffectedByTheOrderingRule();
    TestResolvingAFieldIsNotPlacingContent();
    TestAValidSubgoalSurvivesTheStepsTakenUnderIt();

    std::cout << "The subgoal route asks the subgoal question under its own grammar, a task to "
                 "place\ncontent does not authorise sending it, and a valid subgoal survives the "
                 "steps taken under it.\n";
}
