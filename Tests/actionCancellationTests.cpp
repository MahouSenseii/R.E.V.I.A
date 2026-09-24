#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Goals/goalRunner.h"
#include "Windows/desktopControlExecutor.h"
#include "Windows/windowsAutomationExecutor.h"
#include "reviaSessionTestAccess.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <stdexcept>
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
        return approve ? revia::actions::ConfirmationChoice::Allow
                       : revia::actions::ConfirmationChoice::Decline;
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

void TestAThrowingTurnFailsWithoutWedgingTheSession()
{
    // A turn marks the session busy and clears it only on its way out. One that threw
    // skipped that, so every later voice turn, adapter reply and idle activity waited on
    // busy forever -- and on the voice and adapter threads, which have no caller to catch
    // it, the exception ended the process.
    revia::runtime::ReviaSession session;
    using Access = revia::runtime::ReviaSessionTestAccess;
    const revia::runtime::SessionResult result = Access::GuardTurn(session, [&session]()
        -> revia::runtime::SessionResult
    {
        Access::MarkBusy(session);
        throw std::runtime_error("the block number did not fit");
    });
    Check(!result.succeeded && !result.fromAssistant &&
            result.text.find("could not be completed") != std::string::npos &&
            result.text.find("did not fit") != std::string::npos,
        "A turn that threw was not reported as a failed turn: " + result.text);
    Check(!Access::IsBusy(session), "A turn that threw left the session busy for good.");

    const revia::runtime::SessionResult normal = Access::GuardTurn(session, []()
    {
        revia::runtime::SessionResult ordinary;
        ordinary.succeeded = true;
        ordinary.text = "fine";
        return ordinary;
    });
    Check(normal.succeeded && normal.text == "fine", "The guard changed an ordinary turn.");

    // The background loops get the same rule. A throw is logged, not rethrown, and a stop
    // requested meanwhile still ends the loop promptly rather than after a restart.
    std::stop_source stop;
    int passes = 0;
    bool escaped = false;
    const auto started = std::chrono::steady_clock::now();
    try
    {
        Access::RunBackgroundLoop(session, stop.get_token(), [&]()
        {
            ++passes;
            stop.request_stop();
            throw std::runtime_error("a background pass failed");
        });
    }
    catch (...)
    {
        escaped = true;
    }
    Check(!escaped && passes == 1, "A background loop let an exception leave its thread.");
    Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
        "A stopped background loop waited out its restart pause.");
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
        return approve ? revia::actions::ConfirmationChoice::Allow
                       : revia::actions::ConfirmationChoice::Decline;
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

void TestGuardObligationSurvivesGoalResume()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = PathToUtf8(directory.root / "guarded-goals.db");
    Goal goal;
    goal.id = NewGoalId();
    goal.title = "Resume requires fresh runtime evidence";
    int callbackCalls = 0;
    for (const auto type : {ActionType::InvokeControl, ActionType::ClickPointer})
    {
        GoalStep step;
        step.id = NewStepId();
        step.ordinal = static_cast<std::uint32_t>(goal.steps.size());
        step.action.id = NewActionId();
        step.action.type = type;
        step.action.application = "revia-nonexistent-resume-fixture.exe";
        step.action.control = "Send";
        step.action.requiresRuntimeGuard = true;
        step.action.beforeCommit = [&] { ++callbackCalls; return std::string{}; };
        step.action.onCommitStarted = [&](const std::string&) { ++callbackCalls; };
        step.action.submissionStarted = std::make_shared<std::atomic_bool>(false);
        step.action.navigationConstraint = {true, false, "Send", "Fixture"};
        goal.steps.push_back(std::move(step));
    }
    GoalStep unguarded;
    unguarded.id = NewStepId();
    unguarded.ordinal = static_cast<std::uint32_t>(goal.steps.size());
    unguarded.action.type = ActionType::InspectWindow;
    goal.steps.push_back(std::move(unguarded));
    Check(GoalStore(path).Save(goal), "Could not save the guarded resume fixture.");

    // Reopen the actual store so process-local callbacks cannot satisfy the test.
    const auto restored = GoalStore(path).Load(goal.id);
    Check(restored && restored->steps.size() == 3, "The guarded goal did not reload.");
    const auto stopGuard = std::make_shared<revia::policy::DesktopInputGuard>();
    stopGuard->Trip("This test must refuse before polling native input.");
    windows::WindowsAutomationExecutor automation;
    windows::DesktopControlExecutor desktop({}, stopGuard);
    PolicyDecision approved;
    approved.verdict = PolicyVerdict::Allowed;
    for (std::size_t index = 0; index < 2; ++index)
    {
        const auto& request = restored->steps[index].action;
        Check(request.type == goal.steps[index].action.type && request.requiresRuntimeGuard,
            "Goal persistence dropped the resumed action's runtime guard obligation.");
        Check(!request.beforeCommit && !request.onCommitStarted && !request.submissionStarted &&
            !request.navigationConstraint.enabled && request.navigationConstraint.controlName.empty(),
            "Goal persistence restored stale runtime authority with the guard obligation.");
        const auto result = index == 0 ? automation.Execute(request, approved)
                                       : desktop.Execute(request, approved);
        Check(!result.attempted && !result.succeeded &&
            result.message.find("fresh runtime validation") != std::string::npos,
            "A resumed guarded native action did not refuse before touching the desktop.");
    }
    Check(callbackCalls == 0, "Persistence or refused execution invoked a stale runtime callback.");
    Check(!restored->steps[2].action.requiresRuntimeGuard,
        "An ordinary saved action acquired a runtime guard obligation.");

    // Old rows genuinely lack the marker. Rewrite the JSON directly rather than
    // relying on the current writer, which always emits an explicit boolean.
    const auto replaceAction = [&](const nlohmann::json& action)
    {
        sqlite3* database = nullptr;
        const int opened = sqlite3_open(path.c_str(), &database);
        sqlite3_stmt* update = nullptr;
        const int prepared = opened == SQLITE_OK ? sqlite3_prepare_v2(database,
            "UPDATE goal_steps SET action = ? WHERE id = ?;", -1, &update, nullptr) : opened;
        const auto encoded = action.dump();
        int status = prepared;
        if (prepared == SQLITE_OK)
        {
            sqlite3_bind_text(update, 1, encoded.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(update, 2, goal.steps[2].id.c_str(), -1, SQLITE_TRANSIENT);
            status = sqlite3_step(update);
        }
        if (update) sqlite3_finalize(update);
        if (database) sqlite3_close(database);
        Check(status == SQLITE_DONE, "Could not write a legacy runtime-guard fixture.");
    };
    nlohmann::json legacy = {{"type", "inspect_window"}, {"application", "fixture.exe"}};
    replaceAction(legacy);
    const auto older = GoalStore(path).Load(goal.id);
    Check(older && older->steps.size() == 3 &&
        older->steps[2].action.type == ActionType::InspectWindow &&
        !older->steps[2].action.requiresRuntimeGuard,
        "A legacy action without a runtime-guard marker did not retain its default behavior.");
    for (const auto& malformed : {nlohmann::json(nullptr), nlohmann::json("false"),
             nlohmann::json(0), nlohmann::json::array(), nlohmann::json::object()})
    {
        legacy["requires_runtime_guard"] = malformed;
        replaceAction(legacy);
        const auto corrupt = GoalStore(path).Load(goal.id);
        Check(corrupt && corrupt->steps.size() == 3 &&
            corrupt->steps[2].action.requiresRuntimeGuard &&
            !corrupt->steps[2].action.beforeCommit,
            "A malformed persisted runtime-guard marker restored an unguarded action.");
    }
}

} // namespace

void RunActionCancellationTests()
{
    TestAThrowingTurnFailsWithoutWedgingTheSession();
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
    TestGuardObligationSurvivesGoalResume();
    std::cout << "Action cancellation crosses session confirmation, scoped goals and "
        "the final dispatch boundary while retaining completed-action evidence.\n";
}
