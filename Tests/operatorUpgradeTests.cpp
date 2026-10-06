#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Goals/goalRunner.h"
#include "Planning/goalPlanner.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <future>
#include <thread>

void RunOperatorUpgradeTests()
{
    using namespace revia;
    tests::ScopedTestDirectory directory;
    const auto config = directory.root / "capabilities.json";
    {
        std::ofstream output(config);
        output << nlohmann::json(
            {{"mode", "approved_scope"}, {"approvedRoots", {actions::PathToUtf8(directory.root)}}, {"createMissingApprovedRoots", false}})
                      .dump();
    }
    actions::ActionRuntime runtime;
    std::string error;
    tests::Check(runtime.Initialize(config, directory.root / "audit.jsonl", error), error);
    goals::GoalStore store((directory.root / "goals.db").string());
    goals::GoalRunner runner(runtime, store);
    runner.SetStepProvider(
        [](const goals::Goal&, std::uint32_t)
        {
            goals::NextStep next;
            next.finished = true;
            next.reason = "The model claims every requested file exists.";
            return next;
        });
    goals::Goal goal;
    goal.title = "Create a requested file";
    goal.scope = runtime.Settings();
    const auto result = runner.Operate(goal);
    tests::Check(
        result.status != goals::GoalStatus::Succeeded, "A completion proposal without independent runtime evidence became goal success.");
    for (const auto* recovery : {"wait", "reobserve", "need_vision"})
    {
        const auto parsed = planning::GoalPlanner::ParseNextStep(
            nlohmann::json({{"decision", recovery}, {"reason", "The application is still loading."}}).dump());
        tests::Check(parsed.succeeded && !parsed.finished, "A bounded recovery decision was rejected instead of preserved.");
    }

    runner.SetCompletionVerifier(
        [&](const goals::Goal&, std::stop_token)
        {
            return goals::CompletionEvidence{
                std::filesystem::is_regular_file(directory.root / "requested.txt"), "Acceptance checks the owner-requested file directly."};
        });
    tests::Check(runner.Operate(goal).status == goals::GoalStatus::Blocked, "A failed independent acceptance check became completion.");
    {
        std::ofstream requested(directory.root / "requested.txt");
        requested << "owner requested evidence";
    }
    tests::Check(runner.Operate(goal).status == goals::GoalStatus::Succeeded,
        "Actual runtime acceptance evidence could not complete an already satisfied task.");
    int recoveries = 0;
    runner.SetRecoveryHandler(
        [&](goals::NextStep::Recovery kind, const goals::Goal&, std::stop_token token, std::string& evidence)
        {
            tests::Check(!token.stop_requested() && kind != goals::NextStep::Recovery::None, "Recovery lost its kind or cancellation.");
            ++recoveries;
            evidence = "A fresh admitted observation is available.";
            return true;
        });
    runner.SetStepProvider(
        [](const goals::Goal&, const std::uint32_t iteration)
        {
            goals::NextStep next;
            if (iteration == 0)
                next.recovery = goals::NextStep::Recovery::WaitForState;
            else if (iteration == 1)
                next.recovery = goals::NextStep::Recovery::Reobserve;
            else if (iteration == 2)
                next.recovery = goals::NextStep::Recovery::NeedVision;
            else
                next.finished = true;
            return next;
        });
    const auto recovered = runner.Operate(goal);
    tests::Check(recovered.status == goals::GoalStatus::Succeeded && recoveries == 3 && recovered.spend.plannerRequests == 4,
        "Recovery stopped as undecided, skipped fresh observations, or escaped the model budget.");
    runner.SetStepProvider(
        [](const goals::Goal&, std::uint32_t)
        {
            goals::NextStep next;
            next.recovery = goals::NextStep::Recovery::Reobserve;
            return next;
        });
    goal.budget.maxPlannerRequests = 2;
    const auto exhausted = runner.Operate(goal);
    tests::Check(exhausted.status == goals::GoalStatus::Exhausted && exhausted.stopReason == goals::StopReason::BudgetTokens,
        "Repeated recovery requests escaped the planner budget.");
    tests::Check(
        exhausted.spend.plannerRequests == 2, "The recovery loop called its provider beyond the explicit planner request ceiling.");
    runner.SetRecoveryHandler({});
    tests::Check(runner.Operate(goal).status == goals::GoalStatus::Blocked,
        "Recovery without an admitted adapter claimed to have observed the machine.");
    runner.SetStepProvider(
        [](const goals::Goal&, std::uint32_t)
        {
            goals::NextStep next;
            next.recovery = goals::NextStep::Recovery::WaitForState;
            return next;
        });
    std::stop_source stop;
    auto waiting = std::async(std::launch::async, [&] { return runner.Operate(goal, stop.get_token()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    stop.request_stop();
    tests::Check(waiting.get().status == goals::GoalStatus::Cancelled, "A wait recovery swallowed cancellation.");

    const auto schema =
        nlohmann::json::parse(planning::GoalPlanner::NextStepSchema(R"({"scope":{"available_actions":["read_text_file"]}})"));
    const auto encoded = schema.dump();
    tests::Check(encoded.find("execute_process") == std::string::npos && encoded.find("press_keys") == std::string::npos &&
                     encoded.find("read_text_file") != std::string::npos,
        "The model schema advertised tools outside effective permissions.");

    runner.SetCompletionVerifier([](const goals::Goal&, std::stop_token)
        { return goals::CompletionEvidence{false, "The final requested acceptance criterion remains unmet."}; });
    runner.SetStepProvider(
        [&](const goals::Goal&, const std::uint32_t iteration)
        {
            goals::NextStep next;
            if (iteration != 0)
            {
                next.finished = true;
                return next;
            }
            next.hasStep = true;
            next.step.description = "Observe the fixture file";
            next.step.action.type = actions::ActionType::ReadTextFile;
            next.step.action.source = directory.root / "requested.txt";
            next.step.check = next.step.action;
            next.step.expected = "owner requested evidence";
            return next;
        });
    goal.budget.maxPlannerRequests = 10;
    const auto rejected = runner.Operate(goal);
    tests::Check(rejected.status == goals::GoalStatus::Blocked, "An unaccepted iterative goal did not remain blocked.");
    tests::Check(runner.Resume(rejected.id).status != goals::GoalStatus::Succeeded,
        "Resuming an iterative goal bypassed independent final acceptance.");
    auto legacy = rejected;
    legacy.iterative = false;
    tests::Check(store.Save(legacy), "Could not save the legacy iterative fixture.");
    tests::Check(runner.Resume(legacy.id).status != goals::GoalStatus::Succeeded,
        "A legacy row with recorded planner calls bypassed iterative final acceptance.");

    runner.SetCompletionVerifier([](const goals::Goal&, std::stop_token)
        { return goals::CompletionEvidence{true, "The independent fixture criterion is currently satisfied."}; });
    runner.SetStepProvider(
        [](const goals::Goal&, std::uint32_t)
        {
            goals::NextStep next;
            next.finished = true;
            next.costReported = true;
            next.tokens = 11;
            return next;
        });
    goal.budget.maxTokens = 10;
    tests::Check(
        runner.Operate(goal).status == goals::GoalStatus::Exhausted, "A completion returned beyond the token ceiling was accepted.");
    goal.budget.maxTokens = 100;
    goal.budget.maxDurationMs = 100;
    bool called = false;
    runner.SetStepProvider(
        [&](const goals::Goal&, std::uint32_t)
        {
            called = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            goals::NextStep next;
            next.finished = true;
            return next;
        });
    tests::Check(runner.Operate(goal).stopReason == goals::StopReason::BudgetDuration && called,
        "A completion returned after the duration ceiling was accepted.");
    runner.SetStepProvider(
        [](const goals::Goal&, std::uint32_t)
        {
            goals::NextStep next;
            next.finished = true;
            return next;
        });
    runner.SetCompletionVerifier(
        [](const goals::Goal&, std::stop_token)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            return goals::CompletionEvidence{true, "Late fixture acceptance."};
        });
    tests::Check(runner.Operate(goal).stopReason == goals::StopReason::BudgetDuration,
        "A verifier result arriving after the duration ceiling was accepted.");
}
