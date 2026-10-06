#include "testSupport.h"
#include "Planning/structuredActionParser.h"
#include "Actions/actionRuntime.h"
#include "Audit/contentDigest.h"
#include "Goals/goalRunner.h"

#include <chrono>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
using namespace revia;

struct Fixture
{
    tests::ScopedTestDirectory directory;
    actions::ActionRuntime runtime;
    std::filesystem::path executable;

    Fixture()
    {
#ifdef _WIN32
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        executable = std::filesystem::path(path).parent_path() / "ReviaProcessFixture.exe";
#endif
        tests::Check(std::filesystem::exists(executable), "The disposable process fixture executable is missing.");
        const nlohmann::json config = {{"mode", "supervised"}, {"approvedRoots", {actions::PathToUtf8(directory.root)}},
            {"createMissingApprovedRoots", false}, {"autoApproveRiskThrough", "reversible_write"},
            {"process", {{"enabled", true}, {"approvedExecutables", {actions::PathToUtf8(executable)}}, {"maxTimeoutMs", 5000},
                            {"maxOutputBytes", 4096}, {"maxEnvironmentBytes", 128}, {"approvedEnvironmentNames", {"TASK_INPUT"}}}}};
        {
            std::ofstream output(directory.root / "capabilities.json");
            output << config.dump();
        }
        std::string error;
        tests::Check(runtime.Initialize(directory.root / "capabilities.json", directory.root / "audit.jsonl", error), error);
    }

    actions::ActionRequest Request(std::vector<std::string> arguments) const
    {
        actions::ActionRequest request;
        request.id = actions::NewActionId();
        request.type = actions::ActionType::ExecuteProcess;
        request.process.executable = executable;
        request.process.workingDirectory = directory.root;
        request.process.arguments = std::move(arguments);
        request.process.timeoutMs = 3000;
        return request;
    }
};

void TestProcessExecution()
{
    Fixture fixture;
    auto request = fixture.Request({"output", "literal argument", "", "a\"b", "trailing\\", "&& whoami"});
    const auto output = fixture.runtime.Execute(request, true);
    tests::Check(output.result.attempted && !output.Succeeded() && output.result.process && output.result.process->exitCode == 7,
        "A real nonzero child exit was not retained: " + output.Message());
    tests::Check(output.result.process->standardOutput == "[literal argument][][a\"b][trailing\\][&& whoami]" &&
                     output.result.process->standardError == "fixture stderr",
        "Literal process arguments or redirected streams changed.");
    tests::Check(fixture.runtime.Execute(fixture.Request({"cwd"}), true).result.content == fixture.directory.root.generic_string(),
        "The process did not execute in its admitted working directory.");
    const auto flood = fixture.runtime.Execute(fixture.Request({"flood"}), true);
    tests::Check(flood.Succeeded() && flood.result.process->outputTruncated &&
                     flood.result.process->standardOutput.size() + flood.result.process->standardError.size() <= 4096,
        "Output capture was unbounded, failed, or deadlocked the child.");
    auto settings = fixture.runtime.Settings();
    settings.process.maxOutputBytes = 64;
    const auto scoped = fixture.runtime.ExecuteScoped(fixture.Request({"flood"}), policy::CapabilityPolicy(settings), true);
    tests::Check(scoped.Succeeded() && scoped.result.process->outputTruncated &&
                     scoped.result.process->standardOutput.size() + scoped.result.process->standardError.size() <= 64,
        "A narrower goal output limit was lost at dispatch.");
    settings.process.enabled = false;
    tests::Check(
        fixture.runtime.ExecuteScoped(request, policy::CapabilityPolicy(settings), true).policy.verdict == actions::PolicyVerdict::Blocked,
        "A goal scope re-enabled disabled process authority.");
    request.process.environment["UNAPPROVED"] = "value";
    tests::Check(fixture.runtime.Execute(request, true).policy.verdict == actions::PolicyVerdict::Blocked,
        "An unapproved environment name reached the executor.");
    request = fixture.Request({"cwd"});
    request.dryRun = true;
    const auto dry = fixture.runtime.Execute(request, true);
    tests::Check(dry.Succeeded() && !dry.result.attempted && !dry.result.process, "A process dry run started a child.");

    request = fixture.Request({"sleep"});
    request.process.timeoutMs = 100;
    const auto timed = fixture.runtime.Execute(request, true);
    tests::Check(timed.result.process && timed.result.process->timedOut && !timed.Succeeded(), "Process timeout was not recorded.");
    request = fixture.Request({"sleep"});
    std::stop_source stop;
    auto active = std::async(std::launch::async, [&] { return fixture.runtime.Execute(request, true, stop.get_token()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.request_stop();
    const auto cancelled = active.get();
    tests::Check(cancelled.result.process && cancelled.result.process->cancelled && !cancelled.Succeeded(),
        "Cancellation did not interrupt the running process.");
    request = fixture.Request({"sleep"});
    int checks = 0;
    request.beforeEffect = [&](const std::string&) { return ++checks >= 8 ? std::string("Captured admission expired.") : std::string(); };
    const auto revoked = fixture.runtime.Execute(request, true);
    tests::Check(revoked.result.process && revoked.result.process->cancelled, "Running process did not recheck captured admission.");

#ifdef _WIN32
    const auto pidPath = fixture.directory.root / "child.pid";
    request = fixture.Request({"tree", actions::PathToUtf8(pidPath)});
    request.process.timeoutMs = 500;
    const auto tree = fixture.runtime.Execute(request, true);
    tests::Check(tree.result.process && tree.result.process->timedOut, "Child-tree fixture did not time out.");
    DWORD pid = 0;
    std::ifstream receipt(pidPath);
    receipt >> pid;
    tests::Check(pid != 0, "The child-tree fixture did not report its spawned child.");
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (child)
    {
        const auto wait = WaitForSingleObject(child, 2000);
        CloseHandle(child);
        tests::Check(wait == WAIT_OBJECT_0, "A descendant survived the process timeout.");
    }
#endif
}

void TestGuardedFileWrite()
{
    Fixture fixture;
    actions::ActionRequest request;
    request.id = actions::NewActionId();
    request.type = actions::ActionType::WriteTextFile;
    request.source = fixture.directory.root / "note.txt";
    request.value = "original contents";
    request.expectedDigest = "missing";
    tests::Check(fixture.runtime.Execute(request).Succeeded(), "Guarded creation did not write a missing file.");
    request.value = "replacement";
    tests::Check(!fixture.runtime.Execute(request).Succeeded(), "A missing-file expectation overwrote an existing file.");
    request.expectedDigest = audit::ContentDigest("different contents");
    tests::Check(!fixture.runtime.Execute(request).Succeeded(), "A stale content digest overwrote the file.");
    request.expectedDigest = audit::ContentDigest("original contents");
    tests::Check(fixture.runtime.Execute(request).Succeeded(), "A matching content digest did not permit the bounded replacement.");
    request.type = actions::ActionType::ReadTextFile;
    const auto read = fixture.runtime.Execute(request);
    tests::Check(read.Succeeded() && read.result.content == "replacement" &&
                     read.result.entries == std::vector<std::string>{"sha256:" + audit::ContentDigest("replacement")},
        "The replacement or its exact read identity was not returned.");
    request.type = actions::ActionType::WriteTextFile;
    request.source = fixture.directory.root / "cancelled.txt";
    request.expectedDigest = "missing";
    int checks = 0;
    request.beforeEffect = [&](const std::string&) { return ++checks >= 2 ? "Admission was withdrawn." : ""; };
    tests::Check(!fixture.runtime.Execute(request).Succeeded() && !std::filesystem::exists(request.source),
        "A cancelled guarded creation left a new file behind.");

    goals::GoalStore store((fixture.directory.root / "goals.db").string());
    goals::Goal goal;
    goal.id = goals::NewGoalId();
    goal.title = "Preserve a process intention";
    goal.scope = fixture.runtime.Settings();
    goals::GoalStep step;
    step.id = goals::NewStepId();
    step.action = fixture.Request({"output", "argument"});
    step.status = goals::StepStatus::Acting;
    goal.steps.push_back(step);
    goal.status = goals::GoalStatus::Running;
    tests::Check(store.Save(goal), "Could not save the interrupted process goal.");
    const auto loaded = store.Load(goal.id);
    tests::Check(loaded && loaded->steps[0].action.process.arguments == step.action.process.arguments && loaded->scope.process.enabled,
        "Process request or captured scope was lost across restart.");
    goals::GoalRunner runner(fixture.runtime, store);
    const auto resumed = runner.Resume(goal.id);
    tests::Check(resumed.status == goals::GoalStatus::Failed && resumed.stopReason == goals::StopReason::UnverifiedEffect,
        "An interrupted process was repeated after restart without reconciliation.");
}

void TestExplicitTaskProcessDelegation()
{
    Fixture fixture;
    auto settings = fixture.runtime.Settings().process;
    settings.allowTaskExecution = true;
    std::string error;
    tests::Check(fixture.runtime.SetProcessSettings(settings, error), error);
    goals::GoalStore store((fixture.directory.root / "delegated.db").string());
    goals::GoalRunner runner(fixture.runtime, store);
    goals::Goal goal;
    goal.title = "Run the explicitly allowed fixture command";
    goal.scope = fixture.runtime.Settings();
    goals::GoalStep step;
    step.description = "Read cwd";
    step.action = fixture.Request({"cwd"});
    step.check.type = actions::ActionType::ListDirectory;
    step.check.source = fixture.directory.root;
    step.expected = "capabilities.json";
    goal.steps.push_back(step);
    runner.SeedStandingApproval(actions::RiskLevel::ReversibleWrite, true);
    tests::Check(runner.Run(goal).stopReason == goals::StopReason::PolicyBlocked,
        "A saved process setting alone delegated execution to an unseeded task.");
    runner.SeedStandingApproval(actions::RiskLevel::ReversibleWrite, true);
    runner.SeedToolDelegation(true, false);
    tests::Check(
        runner.Run(goal).status == goals::GoalStatus::Succeeded, "An explicitly seeded and permitted process task remained unreachable.");
    runner.SeedStandingApproval(actions::RiskLevel::ReversibleWrite, true);
    tests::Check(runner.Run(goal).stopReason == goals::StopReason::PolicyBlocked, "Task process delegation leaked into a later run.");
    goal.scope.process.allowTaskExecution = false;
    runner.SeedStandingApproval(actions::RiskLevel::ReversibleWrite, true);
    runner.SeedToolDelegation(true, false);
    tests::Check(
        runner.Run(goal).stopReason == goals::StopReason::PolicyBlocked, "A task seed widened its captured process delegation scope.");
    goal.scope.process.allowTaskExecution = true;
    bool revoked = false;
    fixture.runtime.SetDispatchObserver(
        [&](const actions::ActionRequest& action, bool beginning)
        {
            if (beginning && action.type == actions::ActionType::ExecuteProcess && !revoked)
            {
                revoked = true;
                settings.allowTaskExecution = false;
                tests::Check(fixture.runtime.SetProcessSettings(settings, error), error);
            }
        });
    runner.SeedStandingApproval(actions::RiskLevel::ReversibleWrite, true);
    runner.SeedToolDelegation(true, false);
    const auto cancelledGrant = runner.Run(goal);
    tests::Check(cancelledGrant.status != goals::GoalStatus::Succeeded && revoked &&
                     std::none_of(cancelledGrant.steps[0].attempts.begin(), cancelledGrant.steps[0].attempts.end(),
                         [](const auto& attempt) { return attempt.executed; }),
        "A process effect executed after its task delegation was revoked at dispatch.");
}
}

void RunFileWriteUpgradeTests()
{
    revia::planning::StructuredActionParser parser;
    const auto request = parser.ParseJson(
        R"({"action":"write_text_file","source":"C:/workspace/output.txt","content":"requested contents","expected_digest":"missing"})");
    revia::tests::Check(request.succeeded, "The guarded file write request is unsupported: " + request.error);
}

void RunProcessExecutionTests()
{
    TestExplicitTaskProcessDelegation();
    revia::planning::StructuredActionParser parser;
    const auto request = parser.ParseJson(
        R"({"action":"execute_process","executable":"C:/approved/tool.exe","working_directory":"C:/workspace","arguments":["literal argument","","a\"b"],"environment":{"TASK_INPUT":"literal"},"timeout_ms":1000})");
    revia::tests::Check(request.succeeded, "The typed process request is unsupported: " + request.error);
    RunFileWriteUpgradeTests();
#ifdef _WIN32
    TestProcessExecution();
    TestGuardedFileWrite();
#endif
}
