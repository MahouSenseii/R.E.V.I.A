#include "../../Tests/Fixture/qualificationAccess.h"
#include "Memory/longTermMemory.h"
#include "observedLocalModel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>

// Opt-in. Drives the production ReviaSession through scripted multi-turn conversations
// against llama-servers the caller already runs, with memory, identity and the archive on.
// It records what was said and what Revia sent to each model; judging the replies is left
// to the reviewer, because a phrase check cannot tell a joke from an invented life.
namespace
{
using namespace revia::runtime;
using json = nlohmann::json;
using namespace std::chrono_literals;

json ReadJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read " + path.string());
    return json::parse(input);
}

void WriteJson(const std::filesystem::path& path, const json& value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2) << '\n';
    if (!output.good())
        throw std::runtime_error("Cannot write " + path.string());
}

// Every optional host is off; memory, embeddings, identity and the archive stay on, and
// the models are the caller's servers behind the observation proxies.
void IsolateSettings(const std::filesystem::path& path, const int mainPort, const int fastPort, const int embeddingPort,
    const std::string& delegation, const int mainContext, const std::string& mainModel)
{
    json settings = ReadJson(path);
    auto& llm = settings["llm"];
    llm.update({{"host", "127.0.0.1"}, {"port", mainPort}, {"modelName", mainModel}, {"autoStartServer", false},
        {"shutdownServerOnExit", false}, {"autoTune", false}, {"visionEnabled", false}, {"contextSize", mainContext}});
    llm["cloud"]["enabled"] = false;
    auto& intelligence = settings["intelligence"];
    intelligence["fast"].update({{"enabled", fastPort > 0}, {"host", "127.0.0.1"}, {"port", fastPort > 0 ? fastPort : 1}});
    intelligence["expert"]["enabled"] = false;
    settings["embedding"].update(
        {{"enabled", true}, {"host", "127.0.0.1"}, {"port", embeddingPort}, {"autoStartServer", false}, {"shutdownServerOnExit", false}});
    for (const char* host : {"speechRecognition", "vision", "perception", "bargeIn", "image", "video", "performance", "improvement",
             "localApi", "memoryReranker", "memoryNli"})
        settings[host]["enabled"] = false;
    settings["speech"].update({{"enabled", false}, {"speakGreeting", false}});
    settings["presence"].update({{"enabled", false}, {"avatarBridgeEnabled", false}, {"externalAdaptersEnabled", false}});
    settings["initiative"].update({{"enabled", false}, {"curiosityEnabled", false}, {"spontaneousSpeechEnabled", false}});
    settings["conversation"]["delegation"] = delegation;
    settings["responseFilter"]["aiReviewEnabled"] = false;
    settings["mcp"]["servers"] = json::array();
    WriteJson(path, settings);
}

// Memory evaluation runs after the reply. A turn is settled once nothing is queued and
// neither model has been asked anything for a short while.
void WaitForQuiet(ReviaSession& session, const revia::quality::ObservedLocalModel& main, const revia::quality::ObservedLocalModel* fast,
    const std::chrono::seconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    // Three seconds is longer than a small model's memory evaluation and shorter than a
    // 27B's, whose turn then looked settled while the evaluation was still running. A
    // trial of a slow model sets REVIA_TRIAL_QUIET_SECONDS.
    const char* configured = std::getenv("REVIA_TRIAL_QUIET_SECONDS");
    const auto quiet = std::chrono::seconds(std::clamp(configured ? std::atoi(configured) : 3, 1, 60));
    std::size_t seen = 0;
    auto quietSince = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline)
    {
        session.PollBackgroundEvents();
        const std::size_t requests = main.Requests().size() + (fast ? fast->Requests().size() : 0);
        if (requests != seen || ReviaSessionTestAccess::QueuedMemoryWork(session) > 0)
        {
            seen = requests;
            quietSince = std::chrono::steady_clock::now();
        }
        else if (std::chrono::steady_clock::now() - quietSince >= quiet)
        {
            return;
        }
        std::this_thread::sleep_for(100ms);
    }
}

bool Finished(const DelegatedTaskStatus status)
{
    return status != DelegatedTaskStatus::Working && status != DelegatedTaskStatus::Reviewing && status != DelegatedTaskStatus::Reporting;
}

json Workflow(const ReviaSession& session)
{
    const auto snapshot = session.AgentWorkflowSnapshot();
    json nodes = json::array();
    for (const auto& node : snapshot.nodes)
    {
        json attempts = json::array();
        for (const auto& attempt : node.attempts)
            attempts.push_back({{"state", revia::agents::ToString(attempt.state)}, {"verified", attempt.verified},
                {"diagnostic", attempt.diagnostic}, {"providerCalls", attempt.providerCalls}, {"toolCalls", attempt.toolCalls},
                {"artifact", attempt.artifact ? attempt.artifact->id : std::string()}});
        nodes.push_back({{"id", node.id}, {"state", revia::agents::ToString(node.state)}, {"attempts", attempts}});
    }
    json view = {{"state", revia::agents::ToString(snapshot.state)}, {"providerCalls", snapshot.providerCalls},
        {"toolCalls", snapshot.toolCalls}, {"nodes", nodes}, {"result", session.AgentWorkflowResult()}};
    if (const auto task = session.DelegatedTask())
        view["task"] = {{"status", ToString(task->status)}, {"objective", task->objective}, {"access", task->access},
            {"reason", task->reason}, {"detail", task->detail}};
    return view;
}

json Memories(const std::filesystem::path& database)
{
    json rows = json::array();
    if (!std::filesystem::exists(database))
        return rows;
    const longTermMemory store(database.string());
    for (const auto& row : store.Load())
        rows.push_back({{"id", row.id}, {"summary", row.summary}, {"category", row.category}, {"subject", row.subject.entityId}});
    return rows;
}
}

int main(int argc, char** argv)
{
    if (argc != 7)
    {
        std::cerr << "Usage: ReviaConversationTrialLive <runtime-root> <script.json> <output-dir> <main-port> <fast-port|0> "
                     "<embedding-port>\n";
        return 2;
    }
    try
    {
        const auto root = std::filesystem::absolute(argv[1]);
        const auto script = ReadJson(std::filesystem::absolute(argv[2]));
        const auto output = std::filesystem::absolute(argv[3]);
        const int mainUpstream = std::stoi(argv[4]);
        const int fastUpstream = std::stoi(argv[5]);
        const int embeddingPort = std::stoi(argv[6]);
        if (std::filesystem::exists(output))
            throw std::runtime_error("Choose a fresh output directory.");
        std::filesystem::create_directories(output);

        revia::quality::ObservedLocalModel main(mainUpstream);
        std::unique_ptr<revia::quality::ObservedLocalModel> fast;
        if (fastUpstream > 0)
            fast = std::make_unique<revia::quality::ObservedLocalModel>(fastUpstream);
        // The shipped Main context; below 16K no tool, delegate_task included, is offered at all.
        IsolateSettings(root / "Config/settings.json", main.port, fast ? fast->port : 0, embeddingPort,
            script.value("delegation", std::string("off")), script.value("mainContext", 32768),
            // The name the Main server reports. Revia refuses a server that serves another model,
            // so a script that tests a different Main names it here.
            script.value("mainModel", std::string("Qwen3.5-4B-Q4_K_M.gguf")));
        if (script.contains("approvedRoots"))
        {
            // Seeded into the live permissions file on first start; workers read them only.
            json capabilities = ReadJson(root / "Config/capabilities.json");
            capabilities["approvedRoots"] = script.at("approvedRoots");
            WriteJson(root / "Config/capabilities.json", capabilities);
        }
        if (script.contains("capabilities"))
        {
            // A script that tests more than reading says so: these replace the shipped,
            // locked-down values in this trial's own copy only.
            json capabilities = ReadJson(root / "Config/capabilities.json");
            capabilities.merge_patch(script.at("capabilities"));
            WriteJson(root / "Config/capabilities.json", capabilities);
        }
        std::filesystem::current_path(root);

        json transcript = {{"schemaVersion", 1}, {"conversations", json::array()}};
        // Replies Revia delivers outside Submit, such as a delegated task's report.
        std::mutex deliveredMutex;
        json delivered = json::array();
        std::atomic<bool> inSubmit = false;
        const auto clockStart = std::chrono::steady_clock::now();
        std::optional<RuntimeEventBus::SubscriptionId> subscription;
        auto session = std::make_unique<ReviaSession>();
        const auto subscribe = [&]
        {
            subscription = session->Events().Subscribe(
                [&](const RuntimeEvent& event)
                {
                    if (event.kind != RuntimeEventKind::AssistantMessage || inSubmit.load())
                        return;
                    const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - clockStart).count();
                    std::lock_guard lock(deliveredMutex);
                    delivered.push_back({{"atSeconds", at}, {"text", event.message}});
                });
        };
        if (!session->Start())
            throw std::runtime_error("ReviaSession did not start.");
        subscribe();
        transcript["hostsAfterStart"] = ReviaSessionTestAccess::DisabledHosts(*session);
        for (const auto& conversation : script.at("conversations"))
        {
            json record = {{"id", conversation.at("id")}, {"turns", json::array()}};
            if (conversation.value("restartBefore", false))
            {
                session->Events().Unsubscribe(*subscription);
                session->Stop();
                session = std::make_unique<ReviaSession>();
                if (!session->Start())
                    throw std::runtime_error("ReviaSession did not restart.");
                subscribe();
                record["restarted"] = true;
            }
            if (conversation.value("newChat", true))
            {
                std::string error;
                if (!session->StartNewConversation(error))
                    throw std::runtime_error("New conversation refused: " + error);
            }
            const auto submit = [&](const std::string& input, const bool settle)
            {
                const std::size_t mainBefore = main.Requests().size();
                const std::size_t fastBefore = fast ? fast->Requests().size() : 0;
                const auto started = std::chrono::steady_clock::now();
                inSubmit = true;
                const SessionResult answer = session->Submit(input);
                inSubmit = false;
                const double replyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                if (settle)
                    WaitForQuiet(*session, main, fast.get(), 90s);
                const auto relationship = session->CurrentRelationship();
                record["turns"].push_back({{"input", input}, {"reply", answer.text}, {"succeeded", answer.succeeded},
                    {"fromAssistant", answer.fromAssistant}, {"reason", answer.reason}, {"replyMs", replyMs},
                    {"speaker", relationship.displayName}, {"memories", Memories(root / "Memory/revia_memory.db").size()},
                    {"mainRequests", {mainBefore, main.Requests().size()}},
                    {"fastRequests", {fastBefore, fast ? fast->Requests().size() : 0}}});
                std::cout << '[' << conversation.at("id").get<std::string>() << "] you: " << input << "\n  revia: " << answer.text << "\n";
            };
            for (const auto& turn : conversation.at("turns"))
                submit(
                    turn.is_string() ? turn.get<std::string>() : turn.at("input").get<std::string>(), !conversation.contains("delegation"));
            if (conversation.contains("delegation"))
            {
                // Chat must stay usable while the workers run; the report arrives on its own.
                const auto& delegation = conversation.at("delegation");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(delegation.value("waitSeconds", 900));
                std::size_t nextDuring = 0;
                const auto during = delegation.value("duringTask", json::array());
                while (std::chrono::steady_clock::now() < deadline)
                {
                    session->PollBackgroundEvents();
                    const auto task = session->DelegatedTask();
                    // A handoff is confirmed inside the turn, so no task now means none is coming.
                    if (!task || Finished(task->status))
                        break;
                    if (task && nextDuring < during.size() && task->status == DelegatedTaskStatus::Working)
                        submit(during.at(nextDuring++).get<std::string>(), false);
                    std::this_thread::sleep_for(200ms);
                }
                WaitForQuiet(*session, main, fast.get(), 60s);
                record["workflow"] = Workflow(*session);
                std::lock_guard lock(deliveredMutex);
                record["delivered"] = delivered;
            }
            if (conversation.contains("agentStudio"))
            {
                // The owner's own Agent Studio run with permitted tools. Unlike a chat hand-off,
                // its workers may write and run what the capabilities allow.
                const auto& studio = conversation.at("agentStudio");
                const auto begun = std::chrono::steady_clock::now();
                json outcome = {{"objective", studio.at("objective")}};
                std::string error;
                if (!session->StartAgentWorkflow(studio.at("objective").get<std::string>(), AgentProviderMode::LocalWithTools, error))
                    outcome["refused"] = error;
                else
                {
                    const auto deadline = begun + std::chrono::seconds(studio.value("waitSeconds", 600));
                    const auto running = [&]
                    {
                        const auto state = session->AgentWorkflowSnapshot().state;
                        return state == revia::agents::WorkflowState::Running || state == revia::agents::WorkflowState::Waiting ||
                               state == revia::agents::WorkflowState::AwaitingAcceptance;
                    };
                    while (running() && std::chrono::steady_clock::now() < deadline)
                    {
                        session->PollBackgroundEvents();
                        std::this_thread::sleep_for(100ms);
                    }
                    outcome["timedOut"] = running();
                    if (running())
                        session->CancelAgentWorkflow();
                }
                outcome["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();
                record["agentStudio"] = outcome;
                record["workflow"] = Workflow(*session);
                std::cout << '[' << conversation.at("id").get<std::string>() << "] agent studio: " << record["workflow"].value("state", "") << " after "
                          << static_cast<int>(outcome["seconds"].get<double>()) << "s\n  result: " << record["workflow"].value("result", "") << "\n";
            }
            transcript["conversations"].push_back(record);
            WriteJson(output / "transcript.json", transcript);
        }
        transcript["memories"] = Memories(root / "Memory/revia_memory.db");
        json relationships = json::array();
        for (const auto& relationship : session->Relationships())
            relationships.push_back(
                {{"entity", relationship.entityId}, {"name", relationship.displayName}, {"interactions", relationship.interactionCount}});
        transcript["relationships"] = relationships;
        {
            std::lock_guard lock(deliveredMutex);
            transcript["delivered"] = delivered;
        }
        session->Events().Unsubscribe(*subscription);
        session->Stop();
        session.reset();
        main.Close();
        main.SaveTraffic(output / "main");
        if (fast)
        {
            fast->Close();
            fast->SaveTraffic(output / "fast");
        }
        WriteJson(output / "transcript.json", transcript);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Conversation trial unavailable: " << error.what() << '\n';
        return 2;
    }
}
