#include "reviaSessionTestAccess.h"
#include "Core/configManager.h"
#include "Runtime/companionSettings.h"
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

void TestMemoryRerankerSettingsCompatibility()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto settingsPath = fixture.root / "settings.json";
    const configManager loader(settingsPath.string(), (fixture.root / "Profiles").string());
    const auto load = [&](const nlohmann::json& document, appSettings& settings)
    {
        // This fixture does not install an embedding server; its independent
        // auto-start path requirements must not decide a reranker settings check.
        nlohmann::json configured = {{"embedding", {{"enabled", false}}}};
        configured.update(document);
        std::ofstream output(settingsPath, std::ios::trunc);
        output << configured.dump();
        output.close();
        Check(!output.fail(), "Could not write the reranker settings fixture.");
        return loader.LoadSettings(settings);
    };
    const nlohmann::json legacy = nlohmann::json::object();
    appSettings oldSettings;
    Check(load(legacy, oldSettings) && !oldSettings.memoryReranker.enabled,
        "A settings file written before memoryReranker existed no longer loads disabled.");
    appSettings emptySettings;
    Check(load({{"memoryReranker", nlohmann::json::object()}}, emptySettings) &&
              !emptySettings.memoryReranker.enabled,
        "An empty optional reranker block enabled the worker.");

    const nlohmann::json enabled = {{"memoryReranker", {{"enabled", true}}}};
    appSettings defaults;
    Check(load(enabled, defaults) && defaults.memoryReranker.enabled &&
              defaults.memoryReranker.cpuThreads == 4 && defaults.memoryReranker.timeoutMs == 1000 &&
              defaults.memoryReranker.startupTimeoutMs == 30000 &&
              defaults.memoryReranker.pythonExecutable == "ThirdParty/MemoryReranker/.venv/Scripts/python.exe" &&
              defaults.memoryReranker.workerScript == "Tools/Workers/memory_reranker.py" &&
              defaults.memoryReranker.modelDirectory == "Models/MemoryReranker/ms-marco-MiniLM-L6-v2",
        "An enabled reranker block did not keep its documented bounded local defaults.");
    defaults.activeProfile = "reload-profile-sentinel";
    Check(load(legacy, defaults) && !defaults.memoryReranker.enabled &&
              defaults.memoryReranker.pythonExecutable.empty() && defaults.memoryReranker.workerScript.empty() &&
              defaults.memoryReranker.modelDirectory.empty() && defaults.activeProfile == "reload-profile-sentinel",
        "Removing the reranker block from reused settings did not disable its previous worker or changed unrelated settings.");
    for (const int endpoint : {0, 1})
    {
        auto document = enabled;
        document["memoryReranker"]["cpuThreads"] = endpoint ? 16 : 1;
        document["memoryReranker"]["timeoutMs"] = endpoint ? 10000 : 1;
        document["memoryReranker"]["startupTimeoutMs"] = endpoint ? 120000 : 1;
        appSettings loaded;
        Check(load(document, loaded) && loaded.memoryReranker.cpuThreads == (endpoint ? 16 : 1) &&
                  loaded.memoryReranker.timeoutMs == (endpoint ? 10000 : 1) &&
                  loaded.memoryReranker.startupTimeoutMs == (endpoint ? 120000 : 1),
            "A supported reranker settings boundary was refused or changed.");
    }
    std::string invalidAccepted;
    for (const auto& [field, value] : std::vector<std::pair<std::string, nlohmann::json>>{
             {"enabled", "yes"}, {"enabled", 1}, {"cpuThreads", "4"}, {"timeoutMs", nullptr},
             {"startupTimeoutMs", nlohmann::json::array()}, {"pythonExecutable", 1},
             {"workerScript", false}, {"modelDirectory", nlohmann::json::object()},
             {"cpuThreads", 0}, {"cpuThreads", 17}, {"timeoutMs", 0}, {"timeoutMs", 10001},
             {"cpuThreads", 16.5}, {"timeoutMs", 10000.5}, {"startupTimeoutMs", 120000.5},
             {"cpuThreads", 4294967300ULL}, {"timeoutMs", 4294968296ULL}, {"startupTimeoutMs", 4294997296ULL},
             {"startupTimeoutMs", 0}, {"startupTimeoutMs", 120001}, {"pythonExecutable", ""},
             {"workerScript", ""}, {"modelDirectory", ""}})
    {
        auto document = enabled;
        document["memoryReranker"][field] = value;
        appSettings rejected;
        if (load(document, rejected))
            invalidAccepted += " " + field + "=" + value.dump();
    }
    Check(invalidAccepted.empty(), "Invalid memoryReranker values passed configuration validation:" + invalidAccepted);
    for (const auto& block : {nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json("enabled")})
    {
        appSettings rejected;
        Check(!load({{"memoryReranker", block}}, rejected), "A malformed reranker block passed configuration validation.");
    }
}

void TestMemoryNliSettingsCompatibility()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = fixture.root / "settings.json";
    const configManager loader(path.string(), (fixture.root / "Profiles").string());
    const auto load = [&](nlohmann::json document, appSettings& settings)
    {
        document["embedding"] = {{"enabled", false}};
        std::ofstream output(path, std::ios::trunc);
        output << document.dump();
        output.close();
        return !output.fail() && loader.LoadSettings(settings);
    };
    appSettings settings;
    Check(load(nlohmann::json::object(), settings) && !settings.memoryNli.enabled,
        "Legacy settings enabled semantic duplicate scoring.");
    Check(load({{"memoryNli", {{"enabled", true}}}}, settings) && settings.memoryNli.enabled &&
              settings.memoryNli.cpuThreads == 2 && settings.memoryNli.timeoutMs == 2000 &&
              settings.memoryNli.workerScript == "Tools/Workers/memory_nli.py",
        "Explicit NLI configuration lost its bounded defaults.");
    settings.activeProfile = "nli-reload-sentinel";
    Check(load(nlohmann::json::object(), settings) && !settings.memoryNli.enabled &&
              settings.memoryNli.modelDirectory.empty() && settings.activeProfile == "nli-reload-sentinel",
        "Omitting NLI settings on reload retained its worker or changed the profile.");
    for (const auto& [key, value] : std::vector<std::pair<std::string, nlohmann::json>>{
            {"enabled", "true"}, {"cpuThreads", 0}, {"cpuThreads", 9}, {"cpuThreads", 2.5},
            {"cpuThreads", 4294967298ULL}, {"timeoutMs", 0}, {"timeoutMs", 10001},
            {"startupTimeoutMs", 120001}, {"workerScript", ""}, {"modelDirectory", ""},
            {"pythonExecutable", ""}, {"modelDirectory", false}})
    {
        nlohmann::json document = {{"memoryNli", {{"enabled", true}}}};
        document["memoryNli"][key] = value;
        Check(!load(document, settings), "Invalid NLI setting was admitted: " + key);
    }
    for (const auto& block : {nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json("enabled")})
        Check(!load({{"memoryNli", block}}, settings), "Malformed optional NLI block was admitted.");
}

void TestMemoryRerankerArtifactPaths()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto installRoot = fixture.root / "install";
    const auto buildRoot = installRoot / "build/debug";
    const std::filesystem::path python = "ThirdParty/MemoryReranker/.venv/Scripts/python.exe";
    const std::filesystem::path script = "Tools/Workers/memory_reranker.py";
    const std::filesystem::path model = "Models/MemoryReranker/ms-marco-MiniLM-L6-v2";
    for (const auto& root : {fixture.root, installRoot})
    {
        for (const auto& artifact : {python, script})
        {
            std::filesystem::create_directories((root / artifact).parent_path());
            std::ofstream(root / artifact) << "fixture";
        }
        std::filesystem::create_directories(root / model);
    }
    std::filesystem::create_directories(buildRoot / "Config");
    const CompanionPaths paths(buildRoot, {"legacy", "Reranker path fixture", "assistant", true});
    const auto bind = [&]()
    {
        appSettings settings;
        settings.memoryReranker.pythonExecutable = python;
        settings.memoryReranker.workerScript = script;
        settings.memoryReranker.modelDirectory = model;
        BindCompanionSettings(settings, paths);
        return settings.memoryReranker;
    };
    const auto ancestor = bind();
    Check(ancestor.pythonExecutable == (installRoot / python).lexically_normal() &&
              ancestor.workerScript == (installRoot / script).lexically_normal() &&
              ancestor.modelDirectory == (installRoot / model).lexically_normal(),
        "Reranker artifacts did not resolve from the nearest installation above a build output.");
    for (const auto& artifact : {python, script})
    {
        std::filesystem::create_directories((buildRoot / artifact).parent_path());
        std::ofstream(buildRoot / artifact) << "local fixture";
    }
    std::filesystem::create_directories(buildRoot / model);
    const auto local = bind();
    Check(local.pythonExecutable == (buildRoot / python).lexically_normal() &&
              local.workerScript == (buildRoot / script).lexically_normal() &&
              local.modelDirectory == (buildRoot / model).lexically_normal(),
        "A reranker artifact at the build output lost to an ancestor copy.");

    appSettings explicitPaths;
    explicitPaths.memoryReranker.pythonExecutable = installRoot / python;
    explicitPaths.memoryReranker.workerScript = installRoot / script;
    explicitPaths.memoryReranker.modelDirectory = installRoot / model;
    BindCompanionSettings(explicitPaths, paths);
    Check(explicitPaths.memoryReranker.pythonExecutable == ancestor.pythonExecutable &&
              explicitPaths.memoryReranker.workerScript == ancestor.workerScript &&
              explicitPaths.memoryReranker.modelDirectory == ancestor.modelDirectory,
        "Binding rewrote explicitly configured absolute reranker paths.");
    appSettings missing;
    missing.memoryReranker.pythonExecutable = "ThirdParty/missing-reranker/python.exe";
    missing.memoryReranker.workerScript = "Tools/missing-reranker.py";
    missing.memoryReranker.modelDirectory = "Models/missing-reranker";
    BindCompanionSettings(missing, paths);
    Check(missing.memoryReranker.pythonExecutable == (buildRoot / "ThirdParty/missing-reranker/python.exe").lexically_normal() &&
              missing.memoryReranker.workerScript == (buildRoot / "Tools/missing-reranker.py").lexically_normal() &&
              missing.memoryReranker.modelDirectory == (buildRoot / "Models/missing-reranker").lexically_normal(),
        "Absent reranker artifacts were not anchored to the captured installation root for fallback.");
}
}

int main()
{
    try
    {
        TestMemoryRerankerSettingsCompatibility();
        std::cout << "PASS optional reranker settings preserve legacy defaults and reject malformed or unbounded values\n";
        TestMemoryNliSettingsCompatibility();
        std::cout << "PASS optional NLI settings remain disabled, reset on reload and reject malformed values\n";
        TestMemoryRerankerArtifactPaths();
        std::cout << "PASS reranker artifacts resolve from captured build output and installation ancestors\n";
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
        {
            // A build output directory is an install root with Config but no ThirdParty
            // or Models; the repository root above it holds those.
            const auto buildRoot = fixture.root / "cmake-build-debug";
            const auto ancestorServer = fixture.root / "ThirdParty/llama.cpp/llama-server.exe";
            const auto ancestorModel = fixture.root / "Models/fixture-chat.gguf";
            const auto localModel = buildRoot / "Models/fixture-embed.gguf";
            for (const auto& file : {ancestorServer, ancestorModel, localModel, fixture.root / "Models/fixture-embed.gguf"})
            {
                std::filesystem::create_directories(file.parent_path());
                std::ofstream(file) << "fixture";
            }
            std::filesystem::create_directories(buildRoot / "Config");
            appSettings bound;
            bound.llm.serverExecutable = "ThirdParty/llama.cpp/llama-server.exe";
            bound.llm.modelPath = "Models/fixture-chat.gguf";
            bound.embedding.modelPath = "Models/fixture-embed.gguf";
            bound.intelligence.fast.modelPath = "Models/revia-missing-fixture-artifact.gguf";
            bound.speech.pythonExecutable = "python";
            BindCompanionSettings(bound, CompanionPaths(buildRoot, {"legacy", "Build fixture", "assistant", true}));
            Check(std::filesystem::path(bound.llm.serverExecutable) == ancestorServer.lexically_normal() &&
                      std::filesystem::path(bound.llm.modelPath) == ancestorModel.lexically_normal(),
                "Shared artifacts above a build output install root were not found; the language model cannot start.");
            Check(std::filesystem::path(bound.embedding.modelPath) == localModel.lexically_normal(),
                "An artifact present at the install root lost to an ancestor copy.");
            Check(std::filesystem::path(bound.intelligence.fast.modelPath) ==
                      (buildRoot / "Models/revia-missing-fixture-artifact.gguf").lexically_normal(),
                "A missing shared artifact was not reported against the install root.");
            Check(bound.speech.pythonExecutable == "python", "A bare executable name stopped resolving through PATH.");
        }
        std::cout << "PASS shared artifacts resolve from the install root, then its nearest ancestor\n";
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
