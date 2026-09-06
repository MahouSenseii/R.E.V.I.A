#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Goals/goalRunner.h"
#include "reviaSessionTestAccess.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stop_token>

namespace
{

using namespace revia::actions;
using namespace revia::goals;
using revia::tests::Check;

struct Fixture
{
    revia::tests::ScopedTestDirectory directory;
    ActionRuntime runtime;
    GoalStore store{(directory.root / "goals.db").string()};

    Fixture()
    {
        const nlohmann::json settings = {
            {"mode", "supervised"},
            {"approvedRoots", {PathToUtf8(directory.root)}},
            {"autoApproveRiskThrough", "read_only"},
            {"createMissingApprovedRoots", false}};
        {
            std::ofstream file(directory.root / "capabilities.json");
            file << settings.dump();
            Check(file.good(), "Could not write cancellation fixture policy.");
        }
        std::string error;
        Check(runtime.Initialize(directory.root / "capabilities.json",
            directory.root / "audit.jsonl", error), error);
    }

    ActionRequest MakeDirectory() const
    {
        ActionRequest request;
        request.id = NewActionId();
        request.type = ActionType::CreateDirectory;
        request.source = directory.root / "created";
        return request;
    }

    Goal MakeGoal() const
    {
        Goal goal;
        goal.title = "Create a disposable directory";
        // A supervised stored goal reaches confirmation without widening the
        // global roots or automatic risk ceiling.
        goal.scope = runtime.Settings();
        GoalStep step;
        step.description = goal.title;
        step.action = MakeDirectory();
        step.check.id = NewActionId();
        step.check.type = ActionType::ListDirectory;
        step.check.source = directory.root;
        step.expected = "created";
        goal.steps.push_back(std::move(step));
        return goal;
    }
};

void TestBothDispatchEntryPoints(const bool scoped, const bool cancelInObserver)
{
    Fixture fixture;
    std::stop_source stop;
    int begins = 0;
    int ends = 0;
    fixture.runtime.SetDispatchObserver([&](const ActionRequest&, const bool beginning)
    {
        if (beginning)
        {
            ++begins;
            if (cancelInObserver) stop.request_stop();
        }
        else ++ends;
    });
    if (!cancelInObserver) stop.request_stop();
    const auto request = fixture.MakeDirectory();
    const revia::policy::CapabilityPolicy scope(fixture.runtime.Settings());
    const auto outcome = scoped
        ? fixture.runtime.ExecuteScoped(request, scope, true, stop.get_token())
        : fixture.runtime.Execute(request, true, stop.get_token());
    Check(!outcome.result.attempted && !outcome.result.succeeded &&
        !std::filesystem::exists(request.source),
        "A cancelled ActionRuntime dispatch still mutated the filesystem.");
    Check(begins == 1 && ends == 1, "Cancellation left a dispatch observer unbalanced.");
}

void TestCancellationDoesNotRewriteACompletedAction()
{
    Fixture fixture;
    std::stop_source stop;
    fixture.runtime.SetDispatchObserver([&](const ActionRequest&, const bool beginning)
    {
        if (!beginning) stop.request_stop();
    });
    const auto request = fixture.MakeDirectory();
    const auto outcome = fixture.runtime.Execute(request, true, stop.get_token());
    Check(stop.stop_requested() && outcome.result.attempted && outcome.result.succeeded &&
        std::filesystem::is_directory(request.source),
        "Cancellation misreported an action that had already completed.");
}

void TestSessionConfirmation(const bool cancel, const bool approve)
{
    Fixture fixture;
    revia::runtime::ReviaSession session;
    revia::runtime::ReviaSessionTestAccess::PrepareActions(session, fixture.directory.root);
    bool prompted = false;
    session.SetConfirmationHandler([&](const ActionRequest&, const PolicyDecision&)
    {
        prompted = true;
        if (cancel) session.RequestStop();
        return approve;
    });
    const auto request = fixture.MakeDirectory();
    const auto result = revia::runtime::ReviaSessionTestAccess::Execute(session, request);
    const bool shouldExecute = !cancel && approve;
    Check(prompted, "The session fixture did not cross its real confirmation callback.");
    Check(result.succeeded == shouldExecute &&
        std::filesystem::exists(request.source) == shouldExecute,
        "The session executed a cancelled/refused confirmation or lost approved behavior.");
    if (cancel)
        Check(result.text.find("cancelled before execution") != std::string::npos,
            "The session did not report cancellation through its actual action result.");
}

enum class CancelAt { Never, Confirmation, AfterAction, BeforeCheck, AfterVerified };

void TestGoalCancellation(const CancelAt point, const bool approve = true)
{
    Fixture fixture;
    std::stop_source stop;
    GoalRunner runner(fixture.runtime, fixture.store);
    bool prompted = false;
    runner.SetConfirmationHandler([&](const ActionRequest&, const PolicyDecision&)
    {
        prompted = true;
        if (point == CancelAt::Confirmation) stop.request_stop();
        return approve;
    });
    fixture.runtime.SetDispatchObserver([&](const ActionRequest& request, const bool beginning)
    {
        if (point == CancelAt::AfterAction && !beginning &&
            request.type == ActionType::CreateDirectory) stop.request_stop();
    });
    runner.SetProgressHandler([&](const GoalProgress& progress)
    {
        if ((point == CancelAt::BeforeCheck && progress.stepStatus == StepStatus::Verifying) ||
            (point == CancelAt::AfterVerified && progress.stepStatus == StepStatus::Succeeded))
            stop.request_stop();
    });
    const Goal result = runner.Run(fixture.MakeGoal(), stop.get_token());
    Check(prompted, "The goal fixture did not reach confirmation.");
    const bool cancelled = point != CancelAt::Never;
    const bool executed = point != CancelAt::Confirmation && approve;
    const bool verified = executed && (point == CancelAt::Never || point == CancelAt::AfterVerified);
    Check(result.status == (cancelled ? GoalStatus::Cancelled : GoalStatus::Succeeded) &&
        result.stopReason == (cancelled ? StopReason::Cancelled : StopReason::Completed),
        "Goal cancellation did not reach the terminal production status.");
    Check(std::filesystem::exists(fixture.directory.root / "created") == executed,
        "The goal executed after cancellation during confirmation.");
    Check(result.steps.front().attempts.size() == 1, "The cancelled goal retried its step.");
    const auto& attempt = result.steps.front().attempts.front();
    Check(attempt.executed == executed && attempt.verified == verified,
        "Goal cancellation lost the truth about execution or verification.");
    if (!cancelled)
        Check(attempt.failure.empty(), "A successful goal acquired a failure message.");
    Check(result.spend.actions == (executed ? (verified ? 2U : 1U) : 0U),
        "Cancellation dispatched a later verification action or miscounted execution.");
    const auto saved = fixture.store.Load(result.id);
    Check(saved && saved->status == result.status &&
        saved->steps.front().attempts.front().executed == executed &&
        saved->steps.front().attempts.front().verified == verified,
        "The durable goal record disagrees with the cancelled execution.");
}

} // namespace

void RunActionCancellationTests()
{
    for (const bool scoped : {false, true})
        for (const bool fromObserver : {false, true})
            TestBothDispatchEntryPoints(scoped, fromObserver);
    TestCancellationDoesNotRewriteACompletedAction();
    TestSessionConfirmation(true, true);
    TestSessionConfirmation(true, false);
    TestSessionConfirmation(false, true);
    TestSessionConfirmation(false, false);
    TestGoalCancellation(CancelAt::Confirmation, true);
    TestGoalCancellation(CancelAt::Confirmation, false);
    TestGoalCancellation(CancelAt::AfterAction);
    TestGoalCancellation(CancelAt::BeforeCheck);
    TestGoalCancellation(CancelAt::AfterVerified);
    TestGoalCancellation(CancelAt::Never);
    std::cout << "Action cancellation crosses session confirmation, scoped goals and "
        "the final dispatch boundary while retaining completed-action evidence.\n";
}
