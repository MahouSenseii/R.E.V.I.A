#include "reviaSessionTestAccess.h"
#include "Memory/longTermMemory.h"
#include "Resources/runtimeLease.h"

#include <atomic>
#include <cstdlib>
#include <future>
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace revia::runtime;
using revia::tests::Check;

class ScopedLogOverride
{
  public:
    ScopedLogOverride()
    {
        if (const char* value = std::getenv("REVIA_LOG_DIR"))
            previous = value;
        Set("relative-legacy-logs");
    }
    ~ScopedLogOverride()
    {
        Set(previous ? previous->c_str() : nullptr);
    }

  private:
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("REVIA_LOG_DIR", value ? value : "");
#else
        if (value)
            setenv("REVIA_LOG_DIR", value, 1);
        else
            unsetenv("REVIA_LOG_DIR");
#endif
    }
    std::optional<std::string> previous;
};

void SaveMemory(const std::filesystem::path& path, const std::string& text)
{
    memoryDecision finding;
    finding.bSuccess = true;
    finding.bShouldRemember = true;
    finding.summary = text;
    finding.category = "fact";
    longTermMemory store(path.string());
    bool added = false;
    Check(store.Save(finding, added) && added, "Synthetic memory was not saved.");
}

void WaitUntil(ReviaSession& session, const std::function<bool()>& ready)
{
    const auto until = std::chrono::steady_clock::now() + 8s;
    while (!ready() && std::chrono::steady_clock::now() < until)
    {
        session.PollBackgroundEvents();
        std::this_thread::sleep_for(5ms);
    }
    Check(ready(), "The bounded runtime fixture did not reach its declared state.");
}
}

int main()
{
    try
    {
        revia::tests::ScopedTestDirectory fixture;
        std::filesystem::create_directories(fixture.root / "Config/Profiles");
        std::filesystem::create_directories(fixture.root / "Config/CompanionSeeds");
        std::ofstream(fixture.root / "Config/Profiles/assistant.json")
            << R"({"id":"assistant","systemPrompt":"legacy-private-profile-sentinel"})";
        std::ofstream(fixture.root / "Config/CompanionSeeds/assistant.json")
            << R"({"id":"assistant","systemPrompt":"neutral-shipped-seed"})";
        {
            ScopedLogOverride override;
            const auto legacyRoot = fixture.root / "separate-legacy-install";
            std::filesystem::create_directories(legacyRoot);
            ReviaSession legacy(CompanionPaths(legacyRoot, {"legacy", "Legacy fixture", "assistant", true}));
            ReviaSessionTestAccess::Log(legacy, "captured legacy log sentinel");
            const auto log = legacyRoot / "relative-legacy-logs/revia.log";
            Check(std::filesystem::is_regular_file(log),
                "Explicit legacy logger resolved against process cwd rather than its captured root.");
        }
        std::cout << "PASS explicit legacy logs anchor to the captured installation root\n";
        CompanionPaths pathsA(fixture.root, {"companion-a", "Same name", "assistant", false});
        CompanionPaths pathsB(fixture.root, {"companion-b", "Same name", "assistant", false});
        SaveMemory(pathsA.Resolve("Memory/revia_memory.db"), "A sentinel orbit amber");
        SaveMemory(pathsB.Resolve("Memory/revia_memory.db"), "B sentinel orbit blue");
        SaveMemory(fixture.root / "Memory/revia_memory.db", "Legacy sentinel orbit global");
        ReviaSession a(pathsA);
        ReviaSession b(pathsB);
        ReviaSession legacyProfile(CompanionPaths(fixture.root, {"legacy", "Original", "assistant", true}));
        aiProfile oldBlueprint;
        aiProfile newBlueprint;
        Check(ReviaSessionTestAccess::LoadConfiguredProfile(legacyProfile, "assistant", oldBlueprint) &&
                  ReviaSessionTestAccess::LoadConfiguredProfile(a, "assistant", newBlueprint) &&
                  oldBlueprint.systemPrompt == "legacy-private-profile-sentinel" && newBlueprint.systemPrompt == "neutral-shipped-seed",
            "A new companion inherited legacy's private authored profile instead of the neutral shipped seed.");
        std::cout << "PASS actual session profile binding separates legacy authored text and neutral new-companion seeds\n";
        auto aReads = std::async(std::launch::async, [&]() { return a.SearchMemories("orbit", 20); });
        auto bReads = std::async(std::launch::async, [&]() { return b.SearchMemories("orbit", 20); });
        const auto memoriesA = aReads.get();
        const auto memoriesB = bReads.get();
        Check(memoriesA.size() == 1 && memoriesA.front().summary == "A sentinel orbit amber",
            "Session A recall crossed its captured private path.");
        Check(memoriesB.size() == 1 && memoriesB.front().summary == "B sentinel orbit blue",
            "Session B recall crossed its captured private path.");
        std::cout << "PASS simultaneous session recall excludes B and legacy fallback\n";

        revia::resources::RuntimeLease leaseA;
        revia::resources::RuntimeLease leaseB;
        Check(leaseA.TryAcquire(a.Stamp()) && !leaseB.TryAcquire(b.Stamp()),
            "Two companions acquired the same foreground model/device owner.");
        leaseA.Release();
        Check(leaseB.TryAcquire(b.Stamp()), "Foreground ownership did not transfer after release.");
        leaseB.Release();
        std::cout << "PASS foreground runtime lease transfers without duplicate device owners\n";

        std::atomic<bool> entered = false;
        std::atomic<bool> release = false;
        std::atomic<int> reportsA = 0;
        a.Events().Subscribe(
            [&](const RuntimeEvent& event)
            {
                if (event.kind == RuntimeEventKind::AssistantMessage)
                    ++reportsA;
            });
        std::string error;
        Check(ReviaSessionTestAccess::LaunchTask(
                  a, "held origin",
                  [&](std::stop_token)
                  {
                      entered.store(true);
                      while (!release.load())
                          std::this_thread::sleep_for(2ms);
                      revia::goals::Goal finished;
                      finished.title = "held origin";
                      finished.status = revia::goals::GoalStatus::Succeeded;
                      return finished;
                  },
                  error),
            "The delayed-origin task did not launch.");
        const RuntimeStamp old = a.Stamp();
        WaitUntil(b, [&]() { return entered.load(); });
        auto stopping = std::async(std::launch::async, [&]() { a.Stop(); });
        WaitUntil(b, [&]() { return !a.Admits(old); });
        release.store(true);
        stopping.get();
        Check(reportsA.load() == 0 && b.SearchMemories("orbit", 20).size() == 1,
            "A delayed completion published or changed B after session retirement.");
        ReviaSession returned(pathsA);
        Check(!returned.Admits(old) && returned.SearchMemories("orbit", 20).front().summary == "A sentinel orbit amber",
            "A to B to A readmitted old work or lost A's private mind.");
        std::cout << "PASS retired background completion is suppressed and A returns with its own mind\n";

        const auto capabilityFile = fixture.root / "goal-capabilities.json";
        std::ofstream(capabilityFile) << nlohmann::json{{"mode", "approved_scope"},
            {"approvedRoots", {revia::actions::PathToUtf8(fixture.root)}}, {"autoApproveRiskThrough", "reversible_write"},
            {"createMissingApprovedRoots",
                false}}.dump();
        auto& actions = ReviaSessionTestAccess::Actions(returned);
        Check(actions.Initialize(capabilityFile, pathsA.Resolve("Audit/goal-fixture.jsonl"), error), error);
        RuntimeStamp effectOrigin;
        actions.SetDispatchObserver(
            [&](const revia::actions::ActionRequest& request, const bool beginning)
            {
                if (!beginning || request.type != revia::actions::ActionType::CreateDirectory)
                    return;
                effectOrigin = request.authorityStamp;
                revia::policy::AuthorityPermissions restriction;
                restriction.operations = {request.type};
                restriction.roots = {fixture.root};
                Check(!returned.Authority()->Deny({effectOrigin, restriction}).empty(), "The live goal denial was not registered.");
            });
        revia::goals::Goal goal;
        goal.id = "scoped-goal-fixture";
        goal.title = "bounded task authority fixture";
        goal.budget.maxRetriesPerStep = 0;
        goal.budget.maxTotalRetries = 0;
        goal.scope = revia::goals::NarrowScopeForGoal(actions.Settings());
        revia::goals::GoalStep step;
        step.description = "Create disposable directory";
        step.action.type = revia::actions::ActionType::CreateDirectory;
        step.action.source = fixture.root / "denied-goal-directory";
        step.check.type = revia::actions::ActionType::ListDirectory;
        step.check.source = fixture.root;
        step.expected = "denied-goal-directory";
        goal.steps.push_back(step);
        const auto deniedGoal = ReviaSessionTestAccess::RunGoal(returned, goal);
        Check(effectOrigin.SameSession(returned.Stamp()) && !effectOrigin.taskId.empty() && !effectOrigin.attemptId.empty(),
            "The real session goal did not carry its registered task and attempt identity to dispatch.");
        Check(deniedGoal.status != revia::goals::GoalStatus::Succeeded && !std::filesystem::exists(step.action.source),
            "A goal task permission change did not stop its next filesystem effect.");
        actions.SetDispatchObserver({});
        const auto firstRun = effectOrigin;
        actions.SetDispatchObserver(
            [&](const revia::actions::ActionRequest& request, const bool beginning)
            {
                if (!beginning || request.type != revia::actions::ActionType::CreateDirectory)
                    return;
                effectOrigin = request.authorityStamp;
                revia::actions::PolicyDecision allowed;
                allowed.verdict = revia::actions::PolicyVerdict::Allowed;
                Check(!returned.Authority()->Evaluate(firstRun, request, allowed).empty(),
                    "A prior execution scope became live again on resume.");
                revia::policy::AuthorityPermissions restriction;
                restriction.operations = {request.type};
                restriction.roots = {fixture.root};
                Check(!returned.Authority()->Deny({effectOrigin, restriction}).empty(), "The second execution scope could not be denied.");
            });
        const auto secondGoal = ReviaSessionTestAccess::RunGoal(returned, goal);
        Check(effectOrigin.taskId != firstRun.taskId && secondGoal.id == goal.id,
            "A new run reused its ended task identity or changed the persistent goal ID.");
        goal.id.clear();
        const auto generatedGoal = ReviaSessionTestAccess::RunGoal(returned, goal);
        Check(!effectOrigin.taskId.empty() && !generatedGoal.id.empty(), "An empty caller goal ID bypassed task scope registration.");
        actions.SetDispatchObserver({});
        std::cout << "PASS actual goal dispatch retains task identity and observes a live denial before its effect\n";

        ReviaSession throwing(CompanionPaths(fixture.root, {"throwing-companion", "Throw fixture", "assistant", false}));
        const auto throwOrigin = throwing.Stamp();
        std::promise<void> throwEntered;
        std::promise<void> throwRelease;
        auto throwGate = throwRelease.get_future();
        auto staleFailure = std::async(std::launch::async,
            [&]()
            {
                return ReviaSessionTestAccess::GuardTurn(throwing,
                    [&]() -> SessionResult
                    {
                        throwEntered.set_value();
                        throwGate.wait();
                        throw std::runtime_error("outgoing-private-failure-sentinel");
                    });
            });
        throwEntered.get_future().wait();
        throwing.Stop();
        throwRelease.set_value();
        const auto failure = staleFailure.get();
        Check(failure.stamp.SameSession(throwOrigin) && failure.text.empty() && !failure.succeeded,
            "A retired exception result lost its captured origin or exposed its outgoing payload.");
        std::cout << "PASS retired exception completions retain origin and suppress outgoing payloads\n";
        auto& retiredActions = ReviaSessionTestAccess::Actions(throwing);
        Check(retiredActions.Initialize(capabilityFile, throwing.Paths().Resolve("Audit/retired-goal.jsonl"), error), error);
        const auto refusedScope = ReviaSessionTestAccess::RunGoal(throwing, goal);
        Check(refusedScope.status == revia::goals::GoalStatus::Blocked && refusedScope.spend.actions == 0,
            "A retired session bypassed a refused task scope registration.");
        std::cout << "PASS run/resume identities stay fresh and refused scope registration executes no action\n";

        Check(returned.StartAgentWorkflow("Bounded fixture objective", true, error), error);
        Check(!returned.StartAgentWorkflow("Refused provider replacement", false, error), "A running workflow admitted replacement.");
        WaitUntil(returned,
            [&]()
            {
                const auto view = returned.AgentWorkflowSnapshot();
                return view.state == revia::agents::WorkflowState::Paused || view.state == revia::agents::WorkflowState::Failed;
            });
        const auto failed = returned.AgentWorkflowSnapshot();
        Check(failed.parentDecision == revia::agents::ParentDecision::Pending, "A failed diagnostic became parent acceptance.");
        ReviaSessionTestAccess::JoinWorkflow(returned);
        const auto checkpoint = pathsA.Resolve("RuntimeData/Agents/workflow.json");
        std::ofstream(checkpoint.parent_path() / "provider.json") << R"({"workflowId":"different-workflow","demonstration":true})";
        ReviaSession mismatched(pathsA);
        Check(!mismatched.ResumeAgentWorkflow(error), "Recovery admitted provider metadata from a different workflow.");
        std::cout << "PASS recovery rejects mismatched provider identity\n";
        Check(returned.RetryAgentNode("verification", "Revised verification with repaired fixture", "fixture-evidence-repaired-v2", error),
            error);
        WaitUntil(returned, [&]() { return returned.AgentWorkflowSnapshot().state == revia::agents::WorkflowState::Accepted; });
        const auto accepted = returned.AgentWorkflowSnapshot();
        Check(accepted.parentDecision == revia::agents::ParentDecision::Accept && accepted.requests == 5,
            "The runtime did not separately charge recovery, reviewer and companion acceptance.");
        Check(accepted.unreportedRequests == accepted.requests && accepted.reportedTokens == 0,
            "The deterministic provider invented measured model token usage.");
        Check(std::filesystem::is_regular_file(pathsA.Resolve("RuntimeData/Agents/workflow.json")),
            "Workflow checkpoint was not saved under its companion.");
        std::cout << "PASS connected runtime workflow recovers once and reaches separate companion acceptance\n";
        std::cout << "PASS checkpoint and unavailable telemetry remain companion scoped\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
