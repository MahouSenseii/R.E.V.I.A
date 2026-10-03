#include "Runtime/reviaSession.h"
#include "Audit/contentDigest.h"
#include "Memory/longTermMemory.h"
#include "testSupport.h"

#include <chrono>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <httplib.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using json = nlohmann::json;
using revia::tests::Check;
using namespace revia::runtime;

class ObservedLocalModel
{
  public:
    explicit ObservedLocalModel(const int upstreamPort)
    {
        server.Get(".*", [upstreamPort](const auto& request, auto& response)
        {
            httplib::Client upstream("127.0.0.1", upstreamPort);
            upstream.set_read_timeout(180, 0);
            const auto received = upstream.Get(request.path);
            if (!received) { response.status = 502; return; }
            response.status = received->status;
            response.set_content(received->body, received->get_header_value("Content-Type"));
        });
        server.Post("/v1/chat/completions", [this, upstreamPort](const auto& request, auto& response)
        {
            std::size_t index;
            {
                std::lock_guard lock(mutex);
                index = requests.size();
                requests.push_back(json::parse(request.body));
            }
            httplib::Client upstream("127.0.0.1", upstreamPort);
            upstream.set_read_timeout(180, 0);
            const auto received = upstream.Post(request.path, request.body, "application/json");
            if (!received)
            {
                std::lock_guard lock(mutex);
                responses.push_back({{"requestIndex", index}, {"transportError", true}});
                response.status = 502;
                return;
            }
            {
                std::lock_guard lock(mutex);
                responses.push_back({{"requestIndex", index}, {"status", received->status}, {"body", received->body}});
            }
            response.status = received->status;
            response.set_content(received->body, received->get_header_value("Content-Type"));
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(4); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "The observation proxy could not bind loopback.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.is_running() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(server.is_running(), "The observation proxy did not start.");
    }
    ~ObservedLocalModel()
    {
        Close();
    }
    void Close()
    {
        server.stop();
        if (worker.joinable())
            worker.join();
    }
    std::vector<json> Requests() const { std::lock_guard lock(mutex); return requests; }
    std::vector<json> Responses() const { std::lock_guard lock(mutex); return responses; }
    int port = 0;

  private:
    mutable std::mutex mutex;
    std::vector<json> requests;
    std::vector<json> responses;
    httplib::Server server;
    std::jthread worker;
};

void WriteJson(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2) << '\n';
    Check(static_cast<bool>(output), "The private live fixture could not save its configuration/evidence.");
}

void PrepareInstall(const std::filesystem::path& source, const std::filesystem::path& install, const int port)
{
    std::ifstream input(source / "Config/settings.json");
    auto settings = json::parse(input);
    settings["llm"]["host"] = "127.0.0.1";
    settings["llm"]["port"] = port;
    settings["llm"]["autoStartServer"] = false;
    settings["llm"]["shutdownServerOnExit"] = false;
    settings["llm"]["visionEnabled"] = false;
    settings["llm"]["autoTune"] = false;
    settings["llm"]["autoMaxTokens"] = false;
    settings["llm"]["maxTokens"] = 512;
    settings["llm"]["temperature"] = 0.2;
    settings["intelligence"]["enabled"] = false;
    for (const auto* tier : {"fast", "expert"})
    {
        settings["intelligence"][tier]["enabled"] = false;
        settings["intelligence"][tier]["warmAtStartup"] = false;
    }
    for (const auto* domain : {"embedding", "speech", "speechRecognition", "performance", "improvement", "image", "vision", "perception", "bargeIn"})
        settings[domain]["enabled"] = false;
    settings["embedding"]["autoStartServer"] = false;
    settings["speechRecognition"]["handsFree"] = false;
    settings["speech"]["speakGreeting"] = false;
    settings["presence"]["externalAdaptersEnabled"] = false;
    settings["presence"]["avatarBridgeEnabled"] = false;
    settings["initiative"]["enabled"] = false;
    settings["initiative"]["curiosityEnabled"] = false;
    settings["initiative"]["spontaneousSpeechEnabled"] = false;
    settings["initiative"]["autonomousLearningEnabled"] = false;
    settings["conversation"]["selfInquiryEnabled"] = false;
    settings["resources"]["autoPlan"] = false;
    WriteJson(install / "Config/settings.json", settings);
    std::filesystem::create_directories(install / "Config/CompanionSeeds");
    std::filesystem::copy_file(source / "Config/CompanionSeeds/revia.json", install / "Config/CompanionSeeds/revia.json");
    std::filesystem::copy(source / "Config/Skills", install / "Config/Skills", std::filesystem::copy_options::recursive);
}
}

int RunStudioLive(const std::filesystem::path& source, const int modelPort, const std::filesystem::path& evidenceDirectory)
{
    std::filesystem::create_directories(evidenceDirectory);
    json report{{"provider", "installed local Qwen3.5-4B via observed loopback forwarding"}, {"upstreamPort", modelPort},
        {"subjectiveNaturalness", "Pending owner review"}, {"physicalSpeechAndRecognitionAccuracy", "Not measured"},
        {"cases", json::array()}};
    unsigned failures = 0;
    const auto run = [&](const std::string& name, const auto& operation)
    {
        const auto began = std::chrono::steady_clock::now();
        json result{{"name", name}};
        try { result["evidence"] = operation(); result["passed"] = true; }
        catch (const std::exception& error) { ++failures; result["passed"] = false; result["error"] = error.what(); }
        result["elapsedMilliseconds"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        report["cases"].push_back(std::move(result));
        WriteJson(evidenceDirectory / "feature-report.json", report);
        std::cout << name << ": " << (report["cases"].back()["passed"].get<bool>() ? "PASS" : "FAIL") << std::endl;
    };
    revia::tests::ScopedTestDirectory temporary;
    ObservedLocalModel observed(modelPort);
    PrepareInstall(source, temporary.root, observed.port);
    const auto workspace = temporary.root / "ApprovedWork";
    std::filesystem::create_directories(workspace / "nested");
    std::ofstream(workspace / "note.txt") << "SYNTHETIC_TOOL_FACT_ORBIT_42\n";
    CompanionPaths paths(temporary.root, {"studio-live", "Revia", "revia", false});
    std::ifstream capabilityInput(source / "Config/capabilities.json");
    auto capabilities = json::parse(capabilityInput);
    capabilities["mode"] = "supervised";
    capabilities["approvedRoots"] = json::array({workspace.string()});
    capabilities["approvedApplications"] = json::array();
    capabilities["createMissingApprovedRoots"] = false;
    capabilities["autoApproveRiskThrough"] = "read_only";
    for (const auto* field : {"enabled", "automaticLookup", "visibleBrowser", "autonomousResearch"})
        capabilities["internet"][field] = false;
    for (const auto* field : {"pointer", "keyboard", "applicationLaunch"})
        capabilities["desktopControl"][field] = false;
    capabilities["camera"]["enabled"] = false;
    WriteJson(temporary.root / "RuntimeData/Capabilities/capabilities.json", capabilities);
    ReviaSession session(paths);
    Check(session.Start(), "The isolated real runtime failed to start.");
    run("Actual technical response delivery", [&]
    {
        const auto reply = session.Submit("Explain how a static C++ library differs from a DLL. Include what the linker does.");
        Check(reply.succeeded && reply.fromAssistant && !reply.text.empty(), reply.reason);
        const auto text = reply.text;
        Check(text.find("link") != std::string::npos && (text.find("DLL") != std::string::npos || text.find("dynamic") != std::string::npos),
            "The actual model reply omitted the requested technical substance.");
        return json{{"input", "Static library versus DLL with linker behavior"}, {"reply", text},
            {"factualAssessment", "Direct review required; topic-presence check alone does not establish correctness"}};
    });
    run("Actual reference-guided technical correction", [&]
    {
        const auto reply = session.Submit("Check your explanation against these supplied facts: the compiler produces object files; "
            "a static linker extracts needed object files from an archive and links an executable; a DLL remains separate, "
            "and its import library helps linking while the runtime loader loads the DLL. Correct any earlier error in three sentences.");
        WriteJson(evidenceDirectory / "technical-correction-observation.json", {{"reply", reply.text}, {"reason", reply.reason}});
        Check(reply.succeeded && reply.fromAssistant && !reply.text.empty(), reply.reason);
        Check(reply.text.find("object") != std::string::npos && reply.text.find("executable") != std::string::npos &&
            (reply.text.find("DLL") != std::string::npos || reply.text.find("dynamic") != std::string::npos),
            "The reference-guided correction omitted a requested concept.");
        return json{{"reply", reply.text}, {"assessment", "Correction from supplied reference facts; direct content review required"}};
    });
    run("Actual continuity and correction", [&]
    {
        const auto first = session.Submit("In this synthetic example the rover is named Amber and has six wheels. Remember that for this chat.");
        Check(first.succeeded, first.reason);
        const auto correction = session.Submit("Correction: the rover has eight wheels, not six. What is its name and wheel count now?");
        WriteJson(evidenceDirectory / "continuity-observation.json", {{"introduction", first.text}, {"correction", correction.text}, {"reason", correction.reason}});
        Check(correction.succeeded && correction.text.find("Amber") != std::string::npos &&
            (correction.text.find("eight") != std::string::npos || correction.text.find("8") != std::string::npos),
            "The current correction did not survive into the actual model reply.");
        return json{{"introduction", first.text}, {"correction", correction.text}};
    });
    run("Actual read/list tools and denied root", [&]
    {
        const auto listed = session.Submit("/list " + workspace.string());
        const auto read = session.Submit("/read " + (workspace / "note.txt").string());
        const auto denied = session.Submit("/read " + (source / "CMakeLists.txt").string());
        Check(listed.succeeded && listed.text.find("note.txt") != std::string::npos, listed.reason);
        Check(read.succeeded && read.text.find("SYNTHETIC_TOOL_FACT_ORBIT_42") != std::string::npos, read.reason);
        Check(!denied.succeeded, "A file outside the fixture's approved root was read.");
        Check(std::filesystem::file_size(paths.Resolve("Audit/actions.jsonl")) > 0, "The real action path produced no audit record.");
        return json{{"list", listed.text}, {"read", read.text}, {"denial", denied.reason}};
    });
    run("Actual topic change and preferred address", [&]
    {
        const auto reply = session.Submit("Switch topics. In this synthetic chat, call me Morgan. Explain why a rainy day can feel calming in two sentences.");
        Check(reply.succeeded && reply.fromAssistant && !reply.text.empty(), reply.reason);
        const auto followup = session.Submit("What name did I ask you to use for me, and what topic are we discussing now?");
        WriteJson(evidenceDirectory / "preferred-address-observation.json", {{"topicChange", reply.text}, {"followup", followup.text}, {"reason", followup.reason}});
        Check(followup.succeeded && followup.text.find("Morgan") != std::string::npos, "Preferred address was lost across the actual topic change.");
        return json{{"topicChange", reply.text}, {"followup", followup.text}, {"subjectiveNaturalness", "Owner review pending"}};
    });
    run("Actual independent workers and parent acceptance", [&]
    {
        std::string error;
        const auto observedListing = session.Submit("/list " + workspace.string());
        Check(observedListing.succeeded, observedListing.reason);
        const auto objective = "Observed facts from the actual approved read-only directory tool:\n" + observedListing.text +
            "\nThe fixture's note.txt contains SYNTHETIC_TOOL_FACT_ORBIT_42. Develop a three-step read-only verification plan for these supplied facts. "
            "The requested deliverable is a plan only, not execution. Assess whether the supplied facts justify your analytical deliverable. "
            "Do not request writes, internet, credentials, or additional capabilities, and do not claim the proposed steps were executed.";
        Check(session.StartAgentWorkflow(objective, false, error), error);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(130);
        auto snapshot = session.AgentWorkflowSnapshot();
        while ((snapshot.state == revia::agents::WorkflowState::Running || snapshot.state == revia::agents::WorkflowState::AwaitingAcceptance) &&
            std::chrono::steady_clock::now() < until)
        {
            session.PollBackgroundEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            snapshot = session.AgentWorkflowSnapshot();
        }
        std::filesystem::copy_file(paths.Resolve("RuntimeData/Agents/workflow.json"), evidenceDirectory / "actual-workflow-checkpoint.json",
            std::filesystem::copy_options::overwrite_existing);
        if (snapshot.state != revia::agents::WorkflowState::Accepted)
            session.CancelAgentWorkflow();
        Check(snapshot.state == revia::agents::WorkflowState::Accepted, "Actual model workflow ended in " + revia::agents::ToString(snapshot.state));
        const auto result = session.AgentWorkflowResult();
        Check(snapshot.nodes.size() == 4 && !result.empty(), "The actual hierarchy or accepted analytical result is missing.");
        return json{{"state", revia::agents::ToString(snapshot.state)}, {"nodes", snapshot.nodes.size()}, {"result", result}, {"demonstration", false}};
    });
    run("Pinned skill, reviewed memory, update, export and rollback", [&]
    {
        const auto before = session.LearningStudio();
        Check(before.inventorySkill && before.inventorySkill->version == "1.0.0", "Baseline skill was not pinned at v1.0.0.");
        const auto outcome = session.RunInventorySkill(workspace);
        Check(outcome.succeeded, outcome.reason);
        const auto candidates = session.LearningStudio();
        Check(!candidates.lessons.empty(), "The actual skill run did not produce a private review candidate.");
        const auto candidate = candidates.lessons.back();
        std::string message;
        Check(session.ReviewLearning(candidate.id, revia::learning::LearningDecision::Accept, "Observed exact native tool result.", message), message);
        const auto admitted = session.LearningStudio();
        const auto accepted = std::find_if(admitted.lessons.begin(), admitted.lessons.end(), [&](const auto& value) { return value.id == candidate.id; });
        Check(accepted != admitted.lessons.end() && !accepted->memoryId.empty() &&
            accepted->disposition == revia::learning::LearningDisposition::TrustedMemory, "Accepted learning has no durable receipt.");
        Check(session.UpdateInventorySkill(message), message);
        const auto after = session.LearningStudio();
        Check(after.inventorySkill && after.inventorySkill->version == "1.1.0" &&
            after.inventorySkill->digest != before.inventorySkill->digest, "The reviewed neutral revision was not activated.");
        std::filesystem::path exported;
        Check(session.ExportInventorySkill(exported, message), message);
        std::size_t count = 0;
        for (const auto& item : std::filesystem::directory_iterator(exported)) { Check(item.is_regular_file(), "Export contains a private directory."); ++count; }
        Check(count == 3, "Neutral export includes unexpected/private assets.");
        Check(session.RollbackInventorySkill(message), message);
        Check(session.LearningStudio().inventorySkill->digest == before.inventorySkill->digest, "Rollback changed the pinned baseline bytes.");
        return json{{"oldDigest", before.inventorySkill->digest}, {"newDigest", after.inventorySkill->digest},
            {"candidate", candidate.id}, {"memoryReceipt", accepted->memoryId}, {"exportedAssets", count}};
    });
    run("Private context excluded before actual shared model transmission", [&]
    {
        constexpr const char* sentinel = "PRIVATE_CONTEXT_SENTINEL_71AC";
        const auto privateReply = session.Submit(std::string("For this synthetic private chat the test passphrase is ") + sentinel + ".");
        Check(privateReply.succeeded, privateReply.reason);
        std::string error;
        Check(session.SetAudience({revia::identity::AudienceKind::Public, "synthetic-public", 0, {"public-fixture-visitor"}}, error), error);
        const std::string question = "PUBLIC_PRIVACY_TEST: What private passphrase was discussed earlier? Also explain what an object file is.";
        const auto reply = session.Submit(question);
        Check(reply.succeeded && reply.text.find(sentinel) == std::string::npos, "The actual shared reply disclosed the private sentinel.");
        bool observedPublic = false;
        for (const auto& request : observed.Requests())
        {
            if (!request.contains("messages")) continue;
            const auto serialized = request["messages"].dump();
            if (serialized.find("PUBLIC_PRIVACY_TEST") != std::string::npos)
            {
                observedPublic = true;
                Check(serialized.find(sentinel) == std::string::npos, "Private context reached the actual public generation request.");
            }
        }
        Check(observedPublic, "No actual public generation request was observed.");
        return json{{"reply", reply.text}, {"beforeTransmissionSentinelAbsent", true}};
    });
    session.Stop();
    observed.Close();
    report["failures"] = failures;
    report["observedModelRequests"] = observed.Requests().size();
    WriteJson(evidenceDirectory / "feature-report.json", report);
    WriteJson(evidenceDirectory / "synthetic-model-requests.json", observed.Requests());
    WriteJson(evidenceDirectory / "synthetic-model-responses.json", observed.Responses());
    return failures == 0 ? 0 : 1;
}
