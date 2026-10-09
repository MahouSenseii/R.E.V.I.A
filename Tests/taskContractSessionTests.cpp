#include "Core/taskContract.h"
#include "Audit/contentDigest.h"
#include "Core/evidenceRef.h"
#include "Runtime/reviaSession.h"
#include "testSupport.h"
#include <algorithm>
#include <iostream>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>
#include <cstdlib>

namespace revia::runtime
{
struct ReviaSessionTestAccess
{
    static std::shared_ptr<const core::TaskContract> Admit(ReviaSession& session, const std::string& participant, const std::string& task)
    {
        session.started.store(true);
        {
            std::lock_guard lock(session.speakerMutex);
            session.currentSpeakerId = participant;
        }
        auto input = Input(session, "Bounded admitted fixture");
        input.context.stamp.taskId = task;
        input.context.stamp.attemptId = task + "-attempt";
        return session.BuildTurnTaskContract(input);
    }

    static std::shared_ptr<const core::TaskContract> Action(ReviaSession& session, RuntimeStamp stamp, std::stop_token stop = {})
    {
        actions::ActionRequest request;
        request.id = "factory-probe";
        request.type = actions::ActionType::ReadTextFile;
        return session.BuildActionTaskContract(request, stamp, stop);
    }

    static void ConfigureWorker(ReviaSession& session, int port, const std::filesystem::path& root,
        actions::ActionRuntime::DispatchObserver observer)
    {
        llmSettings settings;
        settings.host = "127.0.0.1";
        settings.port = port;
        settings.modelName = "scope-fixture";
        settings.bAutoStartServer = settings.bVisionEnabled = false;
        embeddingSettings embedding;
        embedding.bEnabled = embedding.bAutoStartServer = false;
        session.router.ApplyLLMSettings(settings, embedding, aiProfile{});
        const auto config = root / "worker-capabilities.json";
        std::ofstream(config) << nlohmann::json{{"mode", "supervised"},
            {"approvedRoots", {actions::PathToUtf8(root)}}, {"createMissingApprovedRoots", false},
            {"autoApproveRiskThrough", "reversible_write"}}.dump();
        std::string error;
        tests::Check(session.actionRuntime.Initialize(config, root / "worker-journal.jsonl", error), error);
        session.actionRuntime.SetDispatchObserver(std::move(observer));
    }

    static void JoinWorker(ReviaSession& session) { session.agentWorkflow.Join(); }

    static std::shared_ptr<const core::TaskContract> AdmitVoice(ReviaSession& session)
    {
        session.started.store(true);
        (void)session.relationships.Get("person-alpha");
        std::string error;
        tests::Check(session.relationships.GrantRecognitionConsent("person-alpha", error), error);
        const auto revision = session.relationships.RecognitionConsentRevision("person-alpha");
        tests::Check(revision.has_value(), "voice recognition consent exists");
        tests::Check(session.SetAudience({identity::AudienceKind::Shared, "voice-room", 0, {"person-alpha"}}, error), error);
        agents::InputBatch batch;
        batch.source = agents::InputSource::Voice;
        batch.text = "Run bounded fixture";
        batch.context = session.CaptureInputContext(agents::InputSource::Voice,
            {"person-alpha", identity::SpeakerSource::ConsentedVoice, "fixture-observation", *revision, 0.9F});
        return session.BuildTurnTaskContract(batch);
    }

    static void RevokeConsent(ReviaSession& session)
    {
        std::string error;
        tests::Check(session.relationships.RevokeRecognitionConsent("person-alpha", error), error);
    }

    static actions::ActionOutcome GoalAction(ReviaSession& session, const std::filesystem::path& file, bool cancelled)
    {
        std::stop_source stop;
        ReviaSession::GoalTokenScope scope(session, stop.get_token());
        tests::Check(scope.admitted, "actual goal owner registered");
        if (cancelled) stop.request_stop();
        actions::ActionRequest action;
        action.id = actions::NewActionId();
        action.type = actions::ActionType::WriteTextFile;
        action.source = file;
        action.value = "goal native control";
        action.expectedDigest = "missing";
        return session.actionRuntime.ExecuteFor(scope.stamp, action, true, stop.get_token());
    }

    static agents::InputBatch Input(ReviaSession& session, const std::string& text)
    {
        session.started.store(true);
        agents::InputBatch batch;
        batch.text = text;
        batch.context = session.CaptureInputContext(agents::InputSource::Typed);
        batch.acceptedAt = std::chrono::steady_clock::now();
        return batch;
    }

    static SessionResult Run(ReviaSession& session, const agents::InputBatch& input)
    {
        std::lock_guard lock(session.operationMutex);
        return session.RunTurnLocked(input);
    }

    static void Promote(ReviaSession& session, agents::InputBatch& batch)
    {
        tests::Check(!session.ResolveLocalSpeaker("My name is Mira", batch.context).empty(), "actual speaker promotion");
    }

    static std::shared_ptr<audit::EvidenceJournal> StartJournal(ReviaSession& session, const std::filesystem::path& root)
    {
        const auto config = root / "fixture-capabilities.json";
        {
            std::ofstream output(config);
            output << R"({"approvedRoots":[],"createMissingApprovedRoots":false})";
        }
        std::string error;
        const bool initialized = session.actionRuntime.Initialize(config, root / "fixture-journal.jsonl", error);
        tests::Check(initialized, "initialize real session audit owner: " + error);
        auto journal = session.actionRuntime.EvidenceJournalOwner();
        session.runtimeEvidenceBridge.Start(session.eventBus, journal);
        RuntimeEvent event;
        event.kind = RuntimeEventKind::Warning;
        event.stamp = session.sessionIdentity.Stamp({}, {}, session.companionAuthority->Revision());
        event.message = "private fixture content must remain absent";
        session.eventBus.Publish(event);
        return journal;
    }
};
}

namespace
{
using namespace revia;
using tests::Check;

void SessionContractPropagation()
{
    tests::ScopedTestDirectory directory;
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"fixture", "Fixture", "assistant", false}));
    using Access = runtime::ReviaSessionTestAccess;
    auto input = Access::Input(session, "/help");
    Access::Promote(session, input);
    input.context.stamp.taskId = "original-task-identity";
    input.context.stamp.attemptId = "original-attempt-identity";
    const auto first = Access::Run(session, input);
    Check(first.succeeded && first.taskContract, "actual admitted session result carries a contract");
    Check(core::SameRuntimeStamp(first.taskContract->stamp, input.context.stamp), "session preserves populated original identity fields");
    Check(first.taskContract->scope.participantId == input.context.participantId &&
              first.taskContract->scope.participantSource == input.context.participantSource,
        "contract captures promoted participant provenance");
    Check(!first.taskContract->acceptanceObligations.empty() && !first.taskContract->negativeConstraints.empty(),
        "session explicitly captures obligations and restrictions");
    Check(core::SameRuntimeStamp(first.stamp, first.taskContract->stamp), "outer guard cannot erase task and attempt identity");
    const auto longInput = Access::Input(session, "/help " + std::string(9000, 'x'));
    const auto longResult = Access::Run(session, longInput);
    const auto shortUnknown = Access::Run(session, Access::Input(session, "/not-a-command"));
    Check(longResult.taskContract && longResult.succeeded == shortUnknown.succeeded && longResult.text == shortUnknown.text,
        "long unknown command retains the existing named refusal after admission");
    Check(longResult.taskContract->goal.size() <= 8192, "contract metadata does not copy an unbounded conversation input");
    Check(longResult.taskContract->goal.find(audit::ContentDigest(longInput.text)) != std::string::npos,
        "bounded contract metadata binds the full accepted input bytes");
    auto legacyInput = Access::Input(session, "/help");
    const auto legacy = Access::Run(session, legacyInput);
    Check(legacy.succeeded && legacy.taskContract && !legacy.taskContract->stamp.taskId.empty() &&
              !legacy.taskContract->stamp.attemptId.empty(),
        "legacy accepted input receives explicit host migration identities");
    Check(std::any_of(legacy.taskContract->positiveConstraints.begin(), legacy.taskContract->positiveConstraints.end(),
              [](const auto& text) { return text.find("legacy") != std::string::npos; }),
        "migration remains explicit contract provenance");
    auto staleInput = legacyInput;
    staleInput.context.audience.revision++;
    const auto stale = Access::Run(session, staleInput);
    Check(!stale.succeeded && !stale.taskContract && stale.text.empty(), "stale session input cannot publish a contract or response");
}

void UnknownBackgroundCannotBorrowForeground()
{
    tests::ScopedTestDirectory directory;
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"factory", "Factory", "assistant", false}));
    using Access = runtime::ReviaSessionTestAccess;
    const auto original = Access::Admit(session, "local:alpha", "original-turn");
    Check(original != nullptr, "original input admitted");
    auto background = session.Stamp();
    background.taskId = "unrelated-background";
    background.attemptId = "background-attempt";
    const auto unrelated = Access::Admit(session, "local:beta", "unrelated-foreground");
    Check(unrelated != nullptr, "later input admitted");
    const auto action = Access::Action(session, background);
    if (action)
        std::cerr << "factory adopted participant=" << action->scope.participantId
                  << " cancellation origin=" << action->cancellation.origin.taskId << '\n';
    Check(!action, "unknown background task borrowed latest foreground participant and cancellation ancestry");
    Check(Access::Action(session, unrelated->stamp) != nullptr, "same owning foreground task remains admitted");
    std::stop_source cancelled;
    cancelled.request_stop();
    Check(!Access::Action(session, unrelated->stamp, cancelled.get_token()), "cancelled factory refused");
}

class ProposedWorkerBackend
{
public:
    explicit ProposedWorkerBackend(const std::filesystem::path& file)
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
            { response.set_content(R"({"data":[{"id":"scope-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions", [this, file](const auto&, auto& response)
        {
            nlohmann::json content = {{"tool", nullptr}};
            if (calls.fetch_add(1) == 0)
            {
                content["tool"] = {{"action", "write_text_file"}, {"source", actions::PathToUtf8(file)},
                    {"content", "admitted worker effect"}, {"expected_digest", "missing"}};
                proposed.store(true);
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (!release.load() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            response.set_content(nlohmann::json{{"choices", nlohmann::json::array({
                {{"message", {{"content", content.dump()}}}, {"finish_reason", "stop"}}})}}.dump(), "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "worker backend bound");
        worker = std::jthread([this] { server.listen_after_bind(); });
    }

    ~ProposedWorkerBackend()
    {
        release.store(true);
        server.stop();
        if (worker.joinable()) worker.join();
    }

    int port = 0;
    std::atomic<bool> proposed{false}, release{false};
    std::atomic<unsigned> calls{0};
private:
    httplib::Server server;
    std::jthread worker;
};

void WorkerAdmissionSurvivesOnlyItsOrigin(const std::string& change)
{
    tests::ScopedTestDirectory directory;
    const auto file = directory.root / "worker-effect.txt";
    ProposedWorkerBackend backend(file);
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"worker", "Worker", "assistant", false}));
    using Access = runtime::ReviaSessionTestAccess;
    const auto original = change == "consent" ? Access::AdmitVoice(session) : Access::Admit(session, "local:alpha", "originating-turn");
    std::vector<actions::ActionRequest> dispatched;
    Access::ConfigureWorker(session, backend.port, directory.root, [&](const auto& action, bool before)
        {
            if (before)
            {
                dispatched.push_back(action);
                if (change == "final-effect")
                    (void)Access::Admit(session, "local:beta", "late-unrelated-foreground");
            }
        });
    std::string error;
    Check(session.StartAgentWorkflow("Write the bounded fixture file", runtime::AgentProviderMode::LocalWithTools, error), error);
    const auto workflow = session.AgentWorkflowSnapshot();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!backend.proposed.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!backend.proposed.load())
    {
        backend.release.store(true);
        session.CancelAgentWorkflow();
        Access::JoinWorker(session);
        Check(false, "real AgentStudio worker reached proposed native action");
    }
    if (change == "participant")
        (void)Access::Admit(session, "local:beta", "unrelated-foreground");
    else if (change == "foreground")
    {
        auto input = Access::Input(session, "/help");
        input.context.stamp.taskId = "unrelated-foreground";
        input.context.stamp.attemptId = "unrelated-foreground-attempt";
        const auto reply = Access::Run(session, input);
        Check(reply.succeeded && reply.taskContract && reply.taskContract->stamp.taskId == "unrelated-foreground",
            "real unrelated foreground turn completed while worker was paused");
    }
    else if (change == "audience")
        Check(session.SetAudience({identity::AudienceKind::Shared, "shared-fixture", 0, {"local:beta"}}, error), error);
    else if (change == "cancel")
        session.CancelAgentWorkflow();
    else if (change == "parent")
        session.Authority()->EndTask(workflow.stamp);
    else if (change == "consent")
        Access::RevokeConsent(session);
    backend.release.store(true);
    Access::JoinWorker(session);
    if (const char* evidenceRoot = std::getenv("REVIA_SCOPE_EVIDENCE_DIR"))
    {
        const auto evidence = std::filesystem::path(evidenceRoot) / change;
        std::filesystem::create_directories(evidence);
        const auto journal = directory.root / "worker-journal.jsonl";
        if (std::filesystem::exists(journal))
            std::filesystem::copy_file(journal, evidence / "native-journal.jsonl",
                std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(session.Paths().Resolve("RuntimeData/Agents/tool-scope.json"), evidence / "admission.json",
            std::filesystem::copy_options::overwrite_existing);
        nlohmann::json contracts = nlohmann::json::array();
        for (const auto& action : dispatched)
        {
            std::string serialized;
            if (action.taskContract && core::SerializeTaskContract(*action.taskContract, serialized))
                contracts.push_back(nlohmann::json::parse(serialized));
        }
        std::ofstream(evidence / "dispatch-contracts.json") << contracts.dump(2);
        std::ofstream(evidence / "native-effect.json") << nlohmann::json{{"exists", std::filesystem::exists(file)},
            {"path", actions::PathToUtf8(file)}, {"proposals", backend.calls.load()}}.dump(2);
    }
    std::ifstream capturedInput(session.Paths().Resolve("RuntimeData/Agents/tool-scope.json"));
    const auto capturedJson = nlohmann::json::parse(capturedInput);
    core::TaskContract capturedTask;
    Check(capturedJson.contains("taskContract") && core::DeserializeTaskContract(capturedJson.at("taskContract").dump(), capturedTask),
        "workflow saved its original host task contract");
    Check(core::SameMemoryScope(capturedTask.scope, original->scope) && capturedTask.stamp.taskId == workflow.id &&
        capturedTask.goal == "Write the bounded fixture file", "saved workflow admission retains original scope, identity and objective");
    const bool allowed = change == "unchanged" || change == "foreground";
    std::ifstream input(directory.root / "worker-journal.jsonl");
    std::string line;
    unsigned results = 0, succeeded = 0, denials = 0;
    while (std::getline(input, line))
    {
        const auto record = nlohmann::json::parse(line);
        if (record.value("reason", "") == "contract_admission_refused")
        {
            ++denials;
            core::EvidenceRef denial;
            Check(core::DeserializeEvidenceRef(record.at("evidence").dump(), denial) && denial.scope.participantId.empty() &&
                denial.scope.audience.audienceId == "runtime-system", "refusal receipt stays redacted rather than adopting foreground scope");
        }
        if (record.value("action", "") != "write_text_file" || record.value("record_type", "") != "result") continue;
        ++results;
        succeeded += record.value("succeeded", false) ? 1 : 0;
        std::cerr << "worker receipt case=" << change << " attempted=" << record.value("attempted", false)
                  << " succeeded=" << record.value("succeeded", false) << " reason=" << record.value("result", "") << '\n';
        Check(record.value("attempted", false) == allowed, "receipt preserves exact executor attempted state: " + change);
        core::EvidenceRef evidence;
        Check(record.contains("evidence") && core::DeserializeEvidenceRef(record.at("evidence").dump(), evidence),
            "native receipt has valid captured evidence");
        Check(core::SameMemoryScope(evidence.scope, original->scope), "durable receipt retains original scope: " + change);
    }
    std::cerr << "worker case=" << change << " dispatches=" << dispatched.size() << " effects=" << std::filesystem::exists(file)
              << " result receipts=" << results << " succeeded receipts=" << succeeded << " redacted denials=" << denials << '\n';
    Check(std::filesystem::exists(file) == allowed, "worker native effect retained origin admission: " + change);
    if (allowed)
        Check(results == 1 && succeeded == 1, "one exact successful native receipt: " + change);
    for (const auto& action : dispatched)
    {
        Check(action.taskContract != nullptr, "worker supplies its host contract");
        const auto& contract = *action.taskContract;
        Check(contract.positiveConstraints == capturedTask.positiveConstraints && contract.negativeConstraints == capturedTask.negativeConstraints &&
            contract.resourceCeilings.maximumRequests == capturedTask.resourceCeilings.maximumRequests,
            "worker retains admitted constraints and resource ceiling without foreground inheritance");
        Check(core::SameMemoryScope(contract.scope, original->scope), "worker retained immutable original scope: " + change);
        Check(contract.cancellation.origin.taskId == workflow.id && contract.cancellation.ancestorTaskIds == std::vector<std::string>{workflow.id},
            "worker lineage names only its admitted workflow parent: " + change);
        Check(contract.stamp.taskId != "unrelated-foreground" && contract.stamp.taskId == contract.stamp.attemptId,
            "worker retains actual attempt identity");
    }
}

void RegisteredGoalRetainsNativeSuccess()
{
    tests::ScopedTestDirectory directory;
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"goal", "Goal", "assistant", false}));
    using Access = runtime::ReviaSessionTestAccess;
    (void)Access::Admit(session, "local:alpha", "goal-launch-turn");
    Access::ConfigureWorker(session, 1, directory.root, {});
    const auto success = Access::GoalAction(session, directory.root / "goal-success.txt", false);
    Check(success.Succeeded() && std::filesystem::exists(directory.root / "goal-success.txt"), "registered goal native success: " + success.Message());
    const auto cancelled = Access::GoalAction(session, directory.root / "goal-cancelled.txt", true);
    Check(!cancelled.result.attempted && !std::filesystem::exists(directory.root / "goal-cancelled.txt"), "registered goal cancellation");
}

void SavedWorkflowAdmissionCannotChange(const std::string& change)
{
    tests::ScopedTestDirectory directory;
    ProposedWorkerBackend backend(directory.root / "resume-effect.txt");
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"resume", "Resume", "assistant", false}));
    using Access = runtime::ReviaSessionTestAccess;
    (void)Access::Admit(session, "local:alpha", "resume-origin");
    Access::ConfigureWorker(session, backend.port, directory.root, {});
    std::string error;
    Check(session.StartAgentWorkflow("Preserve original resume admission", runtime::AgentProviderMode::LocalWithTools, error), error);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!backend.proposed.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    session.CancelAgentWorkflow();
    backend.release.store(true);
    Access::JoinWorker(session);
    Check(backend.proposed.load() && !std::filesystem::exists(directory.root / "resume-effect.txt"),
        "resume fixture stopped after proposal before tool dispatch");
    const auto scopeFile = session.Paths().Resolve("RuntimeData/Agents/tool-scope.json");
    std::ifstream input(scopeFile);
    auto saved = nlohmann::json::parse(input);
    input.close();
    if (change == "participant")
    {
        const auto replacement = Access::Admit(session, "local:beta", "resume-unrelated-turn");
        std::string serialized;
        Check(static_cast<bool>(core::SerializeTaskContract(*replacement, serialized)), "replacement fixture scope serialized");
        saved["taskContract"]["scope"] = nlohmann::json::parse(serialized).at("scope");
    }
    else if (change == "positive" || change == "negative")
        saved["taskContract"][change + "Constraints"] = {"Replacement constraint from changed saved data"};
    if (change != "unchanged")
        std::ofstream(scopeFile) << saved.dump();
    const bool resumed = session.ResumeAgentWorkflow(error);
    if (resumed)
    {
        session.CancelAgentWorkflow();
        Access::JoinWorker(session);
    }
    std::cerr << "saved admission case=" << change << " resumed=" << resumed << " reason=" << error << '\n';
    Check(resumed == (change == "unchanged"), "resume must retain exact original host admission: " + change);
}

void SessionShutdownFlushesRedactedLifecycle()
{
    tests::ScopedTestDirectory directory;
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"fixture", "Fixture", "assistant", false}));
    const auto journal = runtime::ReviaSessionTestAccess::StartJournal(session, directory.root);
    auto stamp = session.Stamp();
    stamp.taskId = "runtime-observations";
    stamp.attemptId = "lifecycle";
    memory::MemoryScope systemScope;
    systemScope.companionId = stamp.companionId;
    systemScope.audience = {identity::AudienceKind::Unknown, "runtime-system", 0, {}};
    session.Stop();
    const auto observations = journal->Read({stamp, systemScope, {}, {audit::JournalKind::Observation}});
    Check(!observations.empty(), "actual session shutdown flushes lifecycle evidence");
    for (const auto& observation : observations)
        Check(observation.scope.participantId.empty() && observation.sourceId == "runtime-lifecycle" && observation.observedAtUnixMs > 0,
            "lifecycle uses original observation time and an explicit unattributed system scope");
    std::ifstream input(journal->Path());
    const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Check(saved.find("private fixture content") == std::string::npos, "private event text cannot enter the lifecycle journal");
}

#include "Fixture/taskContractLiveTests.inc"
}

int main(int argc, char** argv)
{
    if (argc == 6 && std::string(argv[1]) == "--live")
    {
        try
        {
            return RunLiveWorker(argv[2], argv[3], argv[4], static_cast<unsigned>(std::stoul(argv[5])));
        }
        catch (const std::exception& error)
        {
            std::cerr << "Live workflow harness failure: " << error.what() << '\n';
            return 2;
        }
    }
    unsigned failures = 0;
    const auto run = [&](const std::string& name, const std::function<void()>& test)
    {
        try
        {
            test();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    run("session propagation", SessionContractPropagation);
    run("unknown background", UnknownBackgroundCannotBorrowForeground);
    run("registered goal", RegisteredGoalRetainsNativeSuccess);
    for (const auto* change : {"unchanged", "participant", "positive", "negative"})
        run(std::string("saved admission ") + change, [=] { SavedWorkflowAdmissionCannotChange(change); });
    for (const auto* change : {"unchanged", "foreground", "participant", "audience", "consent", "cancel", "parent", "final-effect"})
        run(std::string("worker ") + change, [=] { WorkerAdmissionSurvivesOnlyItsOrigin(change); });
    run("shutdown lifecycle", SessionShutdownFlushesRedactedLifecycle);
    return failures ? 1 : 0;
}
