#include "Audit/contentDigest.h"
#include "Core/configManager.h"
#include "Core/localApiToken.h"
#include "Perception/savedFileObservation.h"
#include "Runtime/reviaSession.h"
#include "localModelCredentials.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace
{
using Json = nlohmann::json;
using namespace std::chrono_literals;
using namespace revia::runtime;
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::string Read(const std::filesystem::path& path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "An explicitly selected input could not be read.");
    std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    Require(input.eof() && !input.bad() && bytes.size() <= limit, "An input exceeded its bound or could not be read completely.");
    return bytes;
}
struct Trace
{
    std::ofstream output;
    std::mutex mutex;
    std::vector<std::string> withheld;
    void Scrub(Json& value)
    {
        if (value.is_string())
        {
            auto text = value.get<std::string>();
            for (const auto& secret : withheld)
                if (!secret.empty()) for (std::size_t at = 0; (at = text.find(secret, at)) != std::string::npos; at += 10) text.replace(at, secret.size(), "[withheld]");
            value = std::move(text);
        }
        else if (value.is_structured()) for (auto& item : value) Scrub(item);
    }
    void Emit(Json value)
    {
        std::lock_guard lock(mutex);
        value["recordedAtUnixMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        Scrub(value); output << value.dump() << '\n' << std::flush;
        Require(output.good(), "The new JSONL report could not be retained.");
    }
};
struct WorkingDirectory
{
    std::filesystem::path before = std::filesystem::current_path();
    explicit WorkingDirectory(const std::filesystem::path& root) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::error_code error; std::filesystem::current_path(before, error); }
};
struct StopSession { ReviaSession& session; ~StopSession() { session.Stop(); } };
std::uint64_t AvailableMiB()
{
#ifdef _WIN32
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    Require(GlobalMemoryStatusEx(&memory) != 0, "Physical RAM could not be measured.");
    return memory.ullAvailPhys / (1024 * 1024);
#else
    throw std::runtime_error("This native Windows qualification harness requires a measured Windows RAM preflight.");
#endif
}
double Milliseconds(std::chrono::steady_clock::time_point start)
{ return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
}
int main(int argc, char** argv)
{
    const bool externalEditor = argc == 5 && std::string(argv[4]) == "--external-editor";
    if (argc != 4 && !externalEditor) { std::cerr << "Usage: ReviaCompanionAwarenessLive <fresh-runtime-directory> <new-report.jsonl> <approved-saved.cpp> [--external-editor]\n"; return 2; }
    Trace trace;
    try
    {
        const auto report = std::filesystem::absolute(argv[2]);
        Require(!std::filesystem::exists(report), "Choose a new report path; existing evidence is never replaced.");
        trace.output.open(report, std::ios::binary);
        Require(trace.output.good(), "The new report path could not be opened.");
        const auto root = std::filesystem::canonical(argv[1]);
        const auto source = std::filesystem::canonical(argv[3]);
        trace.Emit({{"kind", "admission"}, {"liveQualified", false}, {"scope", "isolated text-only saved-file awareness"}});
        for (const char* path : {"RuntimeData", "Memory", "Identity", "Logs"})
            Require(!std::filesystem::exists(root / path), "The runtime directory already contains private runtime data.");
        WorkingDirectory cwd(root);
        const auto configuration = Json::parse(Read(root / "Config/settings.json", 1048576));
        const auto capabilities = Json::parse(Read(root / "Config/capabilities.json", 1048576));
        configManager config((root / "Config/settings.json").string(), (root / "Config/Profiles").string());
        appSettings settings; aiProfile profile;
        Require(config.LoadSettings(settings) && config.LoadProfile(settings.activeProfile, profile), "Disposable settings/profile did not load.");
        Require(!profile.bMemoryEnabled && !Json::parse(Read(root / "Config/Profiles" / (settings.activeProfile + ".json"), 1048576)).value("shouldSpeak", true) &&
            !profile.systemPrompt.empty(), "The authored profile copy must disable memory and speech.");
        Require(settings.llm.backend == "LLamaCpp" && settings.llm.host == "127.0.0.1" && settings.llm.bAutoStartServer &&
            settings.llm.bShutdownServerOnExit && !settings.llm.bVisionEnabled && !settings.llm.cloud.bEnabled && settings.llm.logDirectory.empty(),
            "Configure an owned local text-only model with shutdown and no cloud.");
        revia::quality::LocalModelCredentialSource::Validate(settings.llm.apiKey);
        trace.withheld.push_back(settings.llm.apiKey);
        Require(!settings.intelligence.bEnabled && !settings.embedding.bEnabled && !settings.speech.bEnabled &&
            !settings.speechRecognition.bEnabled && !settings.vision.bEnabled && !settings.vision.bContinuousAwareness &&
            !settings.performance.bEnabled && !settings.image.bEnabled && !settings.improvement.bEnabled &&
            !settings.presence.bEnabled && !settings.presence.bAvatarBridgeEnabled && !settings.presence.bExternalAdaptersEnabled && !settings.conversation.bArchiveEnabled &&
            !settings.memoryReranker.enabled && !settings.memoryNli.enabled && settings.mcp.servers.empty(),
            "Disable auxiliary models, audio, screenshots, archive, adapters, media, improvement and MCP in the disposable configuration.");
        Require(settings.perception.bEnabled && settings.initiative.bEnabled && settings.localApi.bEnabled && settings.localApi.bTrustedPrivate &&
            settings.localApi.port != settings.llm.port && settings.localApi.port >= 1024 && settings.llm.port >= 1024 &&
            settings.initiative.mode == "normal" && !settings.initiative.bCuriosityEnabled && !settings.initiative.bAutonomousLearningEnabled && !settings.initiative.bSpontaneousSpeechEnabled &&
            settings.initiative.focusSessionMinutes >= 240 && settings.initiative.returnAfterMinutes >= 1440 &&
            settings.initiative.contextSwitchCount >= 30, "Configure isolated normal initiative, trusted API ports and no incidental window/curiosity cues.");
        Require(capabilities.value("mode", "") == "supervised" && capabilities.value("autoApproveRiskThrough", "") == "read_only" &&
            !capabilities.at("internet").value("enabled", true) && !capabilities.at("guest").value("enabled", true) &&
            !capabilities.at("process").value("enabled", true) && !capabilities.at("browser").value("enabled", true) &&
            !capabilities.at("desktopControl").value("pointer", true) && !capabilities.at("desktopControl").value("keyboard", true) &&
            !capabilities.at("desktopControl").value("applicationLaunch", true) && !capabilities.value("camera", Json::object()).value("enabled", false),
            "The disposable capability policy must allow approved reads without network, guest, browser or process authority.");
        bool insideApprovedRoot = false;
        for (const auto& approved : capabilities.at("approvedRoots"))
        {
            const auto relative = source.lexically_relative(std::filesystem::canonical(approved.get<std::string>()));
            insideApprovedRoot |= !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
        }
        Require(insideApprovedRoot, "The explicitly selected saved file is outside approved roots.");
        const auto sourceBytes = Read(source, 16384);
        const auto digest = revia::audit::ContentDigest(sourceBytes);
        revia::perception::SavedFileObservation observed{source.parent_path(), source, digest, sourceBytes, std::chrono::system_clock::now()};
        std::string sourceReason;
        Require(revia::perception::ValidateSavedFileObservation(observed, observed.occurredAt, sourceReason), "The approved source is not a complete bounded saved C/C++ observation.");
        trace.withheld.push_back(sourceBytes);
        const auto freeMiB = AvailableMiB();
        const auto modelMiB = std::filesystem::file_size(settings.llm.modelPath) / (1024 * 1024);
        // This text-only probe permits a small installed model with 1 GiB spare.
        // Production placement and optional-work admission remain authoritative.
        const std::uint64_t minimumRamMiB = modelMiB <= 768 ? 1024 : 2048;
        trace.Emit({{"kind", "preflight"}, {"availableRamMiB", freeMiB}, {"minimumRamMiB", minimumRamMiB},
            {"modelFileMiB", modelMiB}, {"sourceSha256", digest},
            {"settingsSha256", revia::audit::ContentDigest(configuration.dump())},
            {"profileSha256", revia::audit::ContentDigest(Read(root / "Config/Profiles" / (settings.activeProfile + ".json"), 1048576))}});
        Require(freeMiB >= minimumRamMiB, "NOT VERIFIED: insufficient measured RAM for the selected text-only model probe; the model was not started.");
        for (const int port : {settings.llm.port, settings.localApi.port})
        { httplib::Client existing("127.0.0.1", port); existing.set_connection_timeout(1); existing.set_read_timeout(1); Require(!existing.Get("/health"), "A selected port already has a responder; no foreign service will be driven."); }
        // Create this fresh runtime's credential through its existing protected-token
        // owner. Never interpret the Windows DPAPI file as plaintext or rotate it
        // after the API has started.
        std::string tokenError;
        const auto token = revia::core::LoadOrCreateLocalApiToken(root / "RuntimeData/LocalApi/api-token", tokenError);
        Require(revia::core::IsLocalApiToken(token), "The fresh runtime's protected API credential could not be prepared.");
        trace.withheld.push_back(token);
        std::atomic<unsigned> comments{0};
        std::atomic<unsigned> savedReceipts{0};
        std::atomic<bool> choseSilence{false};
        std::atomic<std::int64_t> editorBeganNs{0};
        ReviaSession session; StopSession stop{session};
        session.Events().Subscribe([&](const RuntimeEvent& event)
        {
            const bool comment = event.kind == RuntimeEventKind::AssistantMessage && event.component == "Initiative";
            if (comment) ++comments;
            if (event.component == "Editor awareness" && event.phase == "Saved file") ++savedReceipts;
            if (event.component == "Initiative" && event.phase == "Silent") choseSilence = true;
            if (comment || event.component == "Initiative" || event.component == "Editor awareness" || event.kind == RuntimeEventKind::Timing)
                trace.Emit({{"kind", "native_event"}, {"component", event.component}, {"phase", event.phase},
                    {"text", event.message}, {"trace", comment ? event.detail : std::string{}},
                    {"sinceEditorNotificationMs", editorBeganNs.load() > 0
                        ? Json((std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() -
                            editorBeganNs.load()) / 1000000.0) : Json(nullptr)},
                    {"turnId", event.turnId}, {"elapsedMs", event.elapsedMilliseconds}});
        });
        const auto startup = std::chrono::steady_clock::now();
        Require(session.Start(), "NOT VERIFIED: the isolated native session did not start.");
        httplib::Client model("127.0.0.1", settings.llm.port);
        model.set_connection_timeout(3); model.set_read_timeout(3); model.set_default_headers({{"Authorization", "Bearer " + settings.llm.apiKey}});
        const auto verifyModel = [&]
        {
            const auto health = model.Get("/health"), inventory = model.Get("/v1/models");
            Require(health && health->status == 200 && inventory && inventory->status == 200, "Authenticated model health/identity endpoints failed.");
            const auto models = Json::parse(inventory->body).at("data");
            const auto observedModel = models.size() == 1 ? models.front().value("id", "") : std::string{};
            // The owned llama server may advertise the absolute model filename.
            // Match the same filename contract used by the normal runtime probe.
            const bool expected = !observedModel.empty() && (observedModel == settings.llm.modelName ||
                std::filesystem::path(observedModel).filename() == std::filesystem::path(settings.llm.modelPath).filename());
            trace.Emit({{"kind", "model_endpoints"}, {"healthStatus", health->status}, {"modelsStatus", inventory->status},
                {"observedModel", observedModel}, {"expectedModelObserved", expected}});
            Require(expected, "The actual endpoint did not identify the expected model.");
        };
        verifyModel();
        Require(!session.IsVirtualMachineEnabled(), "The disposable runtime unexpectedly enabled a VM.");
        trace.Emit({{"kind", "started"}, {"elapsedMs", Milliseconds(startup)}, {"model", settings.llm.modelName},
            {"load", revia::resources::ToString(session.CurrentLoad().state)}, {"loadReason", session.CurrentLoad().reason},
            {"optionalWorkAllowed", session.CurrentLoad().allowOptionalBackgroundWork}, {"resources", session.ResourceUsage().Summary()}});
        Require(session.CurrentLoad().allowOptionalBackgroundWork, "NOT VERIFIED: normal resource admission blocks optional work.");
        httplib::Client api("127.0.0.1", settings.localApi.port);
        api.set_connection_timeout(3); api.set_read_timeout(10); api.set_default_headers({{"Authorization", "Bearer " + token}});
        const auto post = [&]
        {
            const Json body{{"source", "saved_file"}, {"projectRoot", source.parent_path().generic_string()}, {"path", source.generic_string()},
                {"revision", revia::audit::ContentDigest(Read(source, 16384))}, {"occurredAtUnixMs", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
            const auto began = std::chrono::steady_clock::now(); const auto response = api.Post("/v1/editor/observations", body.dump(), "application/json");
            Require(static_cast<bool>(response), "NOT VERIFIED: the native editor route did not answer.");
            auto receipt = Json::parse(response->body); trace.Emit({{"kind", "editor_receipt"}, {"status", response->status}, {"elapsedMs", Milliseconds(began)}, {"receipt", receipt}});
            return std::pair{response->status, receipt};
        };
        if (externalEditor)
            trace.Emit({{"kind", "external_editor_wait"}, {"expectation", "an independently started exact-file watcher must submit the initial saved event"}});
        else
        {
            editorBeganNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            Require(post().first == 202, "NOT VERIFIED: the real saved-file event was refused.");
        }
        const auto deadline = std::chrono::steady_clock::now() + 90s;
        while (comments.load() == 0 && !choseSilence.load() && std::chrono::steady_clock::now() < deadline) { session.PollBackgroundEvents(); std::this_thread::sleep_for(100ms); }
        const auto firstCount = comments.load();
        Require(!externalEditor || savedReceipts.load() > 0, "NOT VERIFIED: the external watcher did not deliver an initial saved-file event.");
        trace.Emit({{"kind", "opening_observation"}, {"nativeCommentCount", firstCount}, {"nativeSilenceObserved", choseSilence.load()}, {"qualitativeFindingAccepted", nullptr}, {"rawBackendReply", "not exposed through public session API"}});
        if (firstCount > 0)
        {
            const auto began = std::chrono::steady_clock::now();
            const auto answer = session.Submit("Which function in that saved file are you referring to, and what is the smallest correction?");
            trace.Emit({{"kind", "direct_followup"}, {"succeeded", answer.succeeded}, {"finalReply", answer.text}, {"trace", answer.reasoning}, {"reason", answer.reason}, {"elapsedMs", Milliseconds(began)}});
            Require(answer.succeeded && !answer.text.empty(), "NOT VERIFIED: the real direct follow-up failed.");
        }
        const auto beforeDuplicate = comments.load(); Require(post().first == 202, "The duplicate observation was refused.");
        const auto duplicateEnd = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < duplicateEnd) { session.PollBackgroundEvents(); std::this_thread::sleep_for(100ms); }
        Require(comments.load() == beforeDuplicate, "A duplicate observation produced another native comment.");
        session.SetPerceptionPaused(true); Require(post().first == 409, "A paused observation was not refused."); verifyModel();
        trace.Emit({{"kind", "observed_checks"}, {"duplicateWindowMs", 8000}, {"newNativeCommentsAfterDuplicate", comments.load() - beforeDuplicate},
            {"modelCallCountVerified", false}, {"pauseRefused", true}, {"focusScenario", "not exercised"},
            {"load", revia::resources::ToString(session.CurrentLoad().state)}, {"resources", session.ResourceUsage().Summary()}});
        session.Stop();
        trace.Emit({{"kind", "complete"}, {"liveQualified", false}, {"personalityReview", "pending independent human review"},
            {"observation", firstCount ? "actual model comment and follow-up retained" : "no comment observed; silence or suppression remains unqualified"}, {"audioAndGuest", "not exercised"}});
        return firstCount ? 0 : 2;
    }
    catch (const std::exception& error)
    {
        if (trace.output.is_open()) try { trace.Emit({{"kind", "blocked"}, {"status", "NOT VERIFIED"}, {"liveQualified", false}, {"reason", error.what()}}); } catch (...) { }
        std::cerr << "NOT VERIFIED; inspect the new JSONL report when available.\n"; return 2;
    }
}
