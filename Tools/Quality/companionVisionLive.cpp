#include "Audit/contentDigest.h"
#include "Core/configManager.h"
#include "Runtime/reviaSession.h"
#include "localModelCredentials.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Json = nlohmann::json;
using namespace revia::runtime;

void Require(const bool value, const std::string& reason)
{
    if (!value)
        throw std::runtime_error(reason);
}

std::string Read(const std::filesystem::path& path, const std::size_t limit)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "An explicitly selected input could not be read.");
    std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    Require(input.eof() && !input.bad() && bytes.size() <= limit, "An input exceeded its bound or could not be read completely.");
    return bytes;
}

double Milliseconds(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

struct Trace
{
    std::ofstream output;
    std::mutex mutex;
    std::string secret;

    void Scrub(Json& value) const
    {
        if (value.is_string())
        {
            auto text = value.get<std::string>();
            if (!secret.empty())
                for (std::size_t at = 0; (at = text.find(secret, at)) != std::string::npos; at += 10)
                    text.replace(at, secret.size(), "[withheld]");
            value = std::move(text);
        }
        else if (value.is_structured())
            for (auto& item : value)
                Scrub(item);
    }

    void Emit(Json value)
    {
        std::lock_guard lock(mutex);
        value["recordedAtUnixMs"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        Scrub(value);
        output << value.dump() << '\n' << std::flush;
        Require(output.good(), "The new JSONL report could not be retained.");
    }
};

struct WorkingDirectory
{
    std::filesystem::path before = std::filesystem::current_path();
    explicit WorkingDirectory(const std::filesystem::path& root)
    {
        std::filesystem::current_path(root);
    }
    ~WorkingDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(before, error);
    }
};

struct StopSession
{
    ReviaSession& session;
    ~StopSession()
    {
        session.Stop();
    }
};

Json Reference(const conversationMediaReference& reference)
{
    return {{"id", reference.id}, {"digest", reference.digest}, {"name", reference.name}, {"mimeType", reference.mimeType},
        {"width", reference.width}, {"height", reference.height}, {"durationMs", reference.durationMs},
        {"origin", reference.origin == MediaOrigin::GeneratedArtifact ? "generated" : "upload"}};
}

Json References(const std::vector<conversationMediaReference>& references)
{
    auto result = Json::array();
    for (const auto& reference : references)
        result.push_back(Reference(reference));
    return result;
}

bool Inside(const std::filesystem::path& path, const std::filesystem::path& root)
{
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

bool VideoActive(const revia::visual::VideoJobSnapshot& snapshot)
{
    return snapshot.state == "loading" || snapshot.state == "generating" || snapshot.state == "verifying";
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: ReviaCompanionVisionLive <fresh-runtime-directory> <new-report.jsonl> <workflow.json>\n";
        return 2;
    }
    Trace trace;
    try
    {
        const auto root = std::filesystem::canonical(argv[1]);
        const auto report = std::filesystem::absolute(argv[2]);
        const auto workflow = Json::parse(Read(std::filesystem::absolute(argv[3]), 1048576));
        const bool expectVision = workflow.value("expectVisionAvailable", true);
        const bool allowLocalGeneration = workflow.value("allowLocalGeneration", false);
        const bool allowVideoGeneration = workflow.value("allowVideoGeneration", false);
        Require(!std::filesystem::exists(report), "Choose a new report path; existing evidence is never replaced.");
        trace.output.open(report, std::ios::binary);
        Require(trace.output.good(), "The new report path could not be opened.");
        const bool resume = workflow.value("resumeExistingRuntime", false);
        if (resume)
            Require(std::filesystem::is_regular_file(root / "vision-driver-state.json"), "An explicit earlier driver state is required.");
        else
            for (const char* path : {"RuntimeData", "Memory", "Identity", "Logs"})
                Require(!std::filesystem::exists(root / path), "The runtime directory already contains private runtime data.");
        WorkingDirectory cwd(root);
        const auto configuration = Json::parse(Read(root / "Config/settings.json", 1048576));
        configManager config((root / "Config/settings.json").string(), (root / "Config/Profiles").string());
        appSettings settings;
        aiProfile profile;
        Require(config.LoadSettings(settings) && config.LoadProfile(settings.activeProfile, profile),
            "Disposable settings/profile did not load.");
        revia::quality::LocalModelCredentialSource::Validate(settings.llm.apiKey);
        trace.secret = settings.llm.apiKey;
        Require(!profile.bMemoryEnabled && !settings.speech.bEnabled && !settings.speechRecognition.bEnabled &&
                    !settings.embedding.bEnabled && !settings.intelligence.bEnabled && !settings.vision.bEnabled &&
                    !settings.vision.bContinuousAwareness && !settings.perception.bEnabled && !settings.initiative.bEnabled &&
                    (!settings.image.bEnabled || allowLocalGeneration) && (!settings.video.bEnabled || allowVideoGeneration) &&
                    !settings.performance.bEnabled && !settings.improvement.bEnabled && !settings.presence.bEnabled &&
                    !settings.presence.bAvatarBridgeEnabled && !settings.presence.bExternalAdaptersEnabled && !settings.localApi.bEnabled &&
                    settings.mcp.servers.empty(),
            "Disable auxiliary models, audio, screenshots, app awareness, initiative, local API, media, adapters and MCP.");
        Require(!settings.image.bEnabled || (settings.image.host == "127.0.0.1" && settings.image.bOffline &&
                                                settings.image.bShutdownOnExit && !settings.image.bKeepLoaded),
            "Explicit image generation qualification requires the owned offline provider with shutdown and no retained model.");
        Require(settings.llm.host == "127.0.0.1" && settings.llm.backend == "LLamaCpp" && settings.llm.bAutoStartServer &&
                    settings.llm.bShutdownServerOnExit && (!expectVision || settings.llm.bVisionEnabled) && !settings.llm.cloud.bEnabled &&
                    settings.llm.logDirectory.empty() && std::filesystem::is_regular_file(settings.llm.modelPath) &&
                    (!expectVision || std::filesystem::is_regular_file(settings.llm.multimodalProjectorPath)),
            "Select an owned installed local model/projector pair with shutdown and no cloud.");
        httplib::Client existing(settings.llm.host, settings.llm.port);
        existing.set_connection_timeout(1);
        existing.set_read_timeout(1);
        Require(!existing.Get("/health"), "The selected model port already has a responder; no foreign service will be driven.");
        if (settings.image.bEnabled)
        {
            httplib::Client image("127.0.0.1", settings.image.port);
            image.set_connection_timeout(1);
            image.set_read_timeout(1);
            Require(!image.Get("/health"), "The selected image provider port already has a responder.");
        }
        trace.Emit({{"kind", "preflight"}, {"scope", "isolated actual image upload and selected follow-up"}, {"liveQualified", false},
            {"settingsSha256", revia::audit::ContentDigest(configuration.dump())},
            {"profileSha256", revia::audit::ContentDigest(Read(root / "Config/Profiles" / (settings.activeProfile + ".json"), 1048576))},
            {"workflowSha256", revia::audit::ContentDigest(workflow.dump())}, {"model", settings.llm.modelPath},
            {"imageMinTokens", configuration.at("llm").value("imageMinTokens", 0)},
            {"effectiveImageMinTokens", settings.llm.imageMinTokens}, {"projector", settings.llm.multimodalProjectorPath},
            {"modelBytes", std::filesystem::file_size(settings.llm.modelPath)},
            {"projectorBytes", std::filesystem::is_regular_file(settings.llm.multimodalProjectorPath)
                                   ? std::filesystem::file_size(settings.llm.multimodalProjectorPath)
                                   : 0},
            {"expectedVisionAvailable", expectVision}, {"allowLocalGeneration", allowLocalGeneration},
            {"allowVideoGeneration", allowVideoGeneration}});

        std::mutex generatedMutex;
        std::vector<RuntimeEvent> generatedEvents;
        bool generatedOverflow = false;
        ReviaSession session;
        StopSession stop{session};
        session.Events().Subscribe(
            [&](const RuntimeEvent& event)
            {
                if (event.generatedMediaReference)
                {
                    std::lock_guard lock(generatedMutex);
                    if (generatedEvents.size() < 128)
                        generatedEvents.push_back(event);
                    else
                        generatedOverflow = true;
                }
                if (event.component == "Attachments" || event.component == "Intelligence router" || event.component == "Conversation" ||
                    event.kind == RuntimeEventKind::Timing || event.generatedMediaReference)
                    trace.Emit({{"kind", "native_event"}, {"component", event.component}, {"phase", event.phase}, {"text", event.message},
                        {"elapsedMs", event.elapsedMilliseconds}, {"turnId", event.turnId},
                        {"generatedMediaReference",
                            event.generatedMediaReference ? Reference(*event.generatedMediaReference) : Json(nullptr)},
                        {"conversationMediaEpoch", event.conversationMediaEpoch}});
            });
        session.SetConfirmationHandler(
            [&](const revia::actions::ActionRequest& request, const revia::actions::PolicyDecision&)
            {
                const bool allowed = (allowLocalGeneration && request.type == revia::actions::ActionType::GenerateImage) ||
                                     (allowVideoGeneration && request.type == revia::actions::ActionType::GenerateVideo);
                return allowed ? revia::actions::ConfirmationChoice::Allow : revia::actions::ConfirmationChoice::Decline;
            });
        const auto startup = std::chrono::steady_clock::now();
        const bool started = session.Start();
        trace.Emit({{"kind", "session_start"}, {"succeeded", started}, {"elapsedMs", Milliseconds(startup)},
            {"load", revia::resources::ToString(session.CurrentLoad().state)}, {"resources", session.ResourceUsage().Summary()}});
        if (!started && !expectVision)
        {
            trace.Emit({{"kind", "complete"}, {"transportVerified", false}, {"unavailableSessionObserved", true},
                {"semanticQualified", false}, {"qualification", "The configured unavailable vision setup refused startup."}});
            return 0;
        }
        Require(started, "NOT VERIFIED: the isolated native vision session did not start.");
        httplib::Client model(settings.llm.host, settings.llm.port);
        model.set_connection_timeout(3);
        model.set_read_timeout(5);
        model.set_default_headers({{"Authorization", "Bearer " + settings.llm.apiKey}});
        const auto health = model.Get("/health");
        const auto props = model.Get("/props");
        const auto models = model.Get("/v1/models");
        const bool endpoints = health && health->status == 200 && props && props->status == 200 && models && models->status == 200;
        Json properties = endpoints ? Json::parse(props->body) : Json::object();
        const bool vision = properties.value("modalities", Json::object()).value("vision", false);
        trace.Emit({{"kind", "model_endpoints"}, {"healthy", endpoints}, {"visionModality", vision},
            {"models", endpoints ? Json::parse(models->body) : Json(nullptr)},
            {"modalities", properties.value("modalities", Json::object())}});
        Require(!expectVision || (endpoints && vision), "NOT VERIFIED: the actual endpoint did not report an available vision modality.");
        Require(expectVision || !vision, "The configured unavailable vision case unexpectedly advertised a vision modality.");

        std::map<std::string, std::string> ids;
        std::map<std::string, std::filesystem::path> storedFiles;
        std::map<std::string, std::string> conversations;
        if (resume)
        {
            const auto saved = Json::parse(Read(root / "vision-driver-state.json", 1048576));
            ids = saved.at("ids").get<std::map<std::string, std::string>>();
            conversations = saved.at("conversations").get<std::map<std::string, std::string>>();
            for (const auto& [alias, path] : saved.at("storedFiles").items())
                storedFiles.emplace(alias, path.get<std::string>());
            trace.Emit({{"kind", "driver_resume"}, {"separateProcess", true}, {"savedImageIds", ids.size()}});
        }
        const auto artifactDirectory = report.parent_path() / (report.stem().string() + "-generated-artifacts");
        std::map<std::string, conversationMediaReference> generated;
        std::size_t generatedVideos = 0;
        std::size_t waitedVideos = 0;
        const auto captureGenerated = [&](const std::string& alias, const conversationMediaReference& reference, const RuntimeStamp& stamp,
                                          const std::uint64_t audienceRevision, const std::uint64_t mediaEpoch)
        {
            Require(reference.origin == MediaOrigin::GeneratedArtifact && !reference.id.empty() && !reference.digest.empty(),
                "Only a typed host-generated source can create an artifact receipt.");
            const bool video = reference.mimeType == "video/mp4" || reference.mimeType == "video/webm";
            Require(video ? allowVideoGeneration : allowLocalGeneration && reference.mimeType == "image/png",
                "A generated artifact was published without its explicit workflow generation gate.");
            const auto current = [&]
            {
                return !stamp.companionId.empty() && stamp.SameSession(session.Stamp()) &&
                       stamp.policyVersion == session.Stamp().policyVersion && audienceRevision != 0 &&
                       audienceRevision == session.Audience().revision &&
                       session.Audience().kind == revia::identity::AudienceKind::Private && mediaEpoch == session.ConversationMediaEpoch();
            };
            Require(current(), "A generated artifact arrived outside its original private Session/conversation admission.");
            const auto documents = session.Documents();
            const auto document = std::find_if(documents.begin(), documents.end(),
                [&](const auto& item)
                {
                    return item.id == reference.id && item.digest == reference.digest && item.mimeType == reference.mimeType &&
                           item.origin == MediaOrigin::GeneratedArtifact && item.generated.has_value();
                });
            Require(document != documents.end(), "A typed generated source has no matching current canonical library record.");
            const auto path = std::filesystem::canonical(document->storedPath);
            Require(Inside(path, root) && std::filesystem::is_regular_file(path) &&
                        !std::filesystem::is_symlink(std::filesystem::symlink_status(document->storedPath)),
                "The actual generated artifact is not an owned regular file under the isolated runtime.");
            ids[alias] = ids["latest_generated"] = reference.id;
            storedFiles[alias] = storedFiles["latest_generated"] = path;
            if (video)
            {
                ids["latest_generated_video"] = reference.id;
                storedFiles["latest_generated_video"] = path;
            }
            const auto prior = generated.find(reference.id);
            if (prior != generated.end())
            {
                Require(prior->second == reference, "One generated source ID changed its immutable metadata.");
                return;
            }
            const auto bytes = Read(path, 64 * 1024 * 1024);
            Require(bytes.size() == document->bytes && revia::audit::ContentDigest(bytes) == reference.digest,
                "The actual canonical generated bytes do not match the host source digest.");
            if (generated.empty())
            {
                Require(!std::filesystem::exists(artifactDirectory), "Choose a fresh generated-artifact evidence directory.");
                Require(std::filesystem::create_directory(artifactDirectory),
                    "The generated-artifact evidence directory could not be created.");
            }
            const auto copy = artifactDirectory / (std::to_string(generated.size() + 1) + "-" + reference.digest +
                                                      (video ? reference.mimeType == "video/webm" ? ".webm" : ".mp4" : ".png"));
            Require(!std::filesystem::exists(copy), "Existing generated-artifact evidence is never replaced.");
            std::ofstream output(copy, std::ios::binary);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            Require(output.good() && revia::audit::ContentDigest(Read(copy, 64 * 1024 * 1024)) == reference.digest && current(),
                "The bounded generated evidence copy failed or its source admission retired.");
            generated.emplace(reference.id, reference);
            const auto sequence = "generated_" + std::to_string(generated.size());
            ids[sequence] = reference.id;
            storedFiles[sequence] = path;
            generatedVideos += video ? 1 : 0;
            const auto& provenance = *document->generated;
            trace.Emit({{"kind", "generated_artifact"}, {"step", alias}, {"reference", Reference(reference)},
                {"canonicalStoredPath", path.string()}, {"evidencePath", copy.string()}, {"bytes", bytes.size()},
                {"sha256", reference.digest}, {"mode", provenance.mode}, {"provider", provenance.provider}, {"model", provenance.model},
                {"modelRevision", provenance.modelRevision}, {"seed", provenance.seed}, {"sourceId", provenance.sourceId},
                {"sourceDigest", provenance.sourceDigest}, {"settingsDigest", provenance.settingsDigest},
                {"visualQuality", "pending independent content review"}});
        };
        const auto drainGenerated = [&](const std::string& alias)
        {
            std::vector<RuntimeEvent> pending;
            {
                std::lock_guard lock(generatedMutex);
                Require(!generatedOverflow, "Generated event retention exceeded its fixed bound.");
                pending.swap(generatedEvents);
            }
            for (const auto& event : pending)
            {
                Require(event.kind == RuntimeEventKind::Diagram || event.kind == RuntimeEventKind::AssistantMessage,
                    "An unsupported event kind attempted to carry a generated artifact.");
                captureGenerated(alias, *event.generatedMediaReference, event.stamp, event.audienceRevision, event.conversationMediaEpoch);
            }
        };
        unsigned turns = 0;
        for (const auto& step : workflow.at("steps"))
        {
            const auto operation = step.at("operation").get<std::string>();
            const auto name = step.at("name").get<std::string>();
            const auto began = std::chrono::steady_clock::now();
            const auto expectedGenerated = step.find("requireGeneratedCount");
            if (expectedGenerated != step.end())
                Require(expectedGenerated->is_number_integer() &&
                            (expectedGenerated->is_number_unsigned()
                                    ? expectedGenerated->get<std::uint64_t>() <= 16
                                    : expectedGenerated->get<std::int64_t>() >= 0 && expectedGenerated->get<std::int64_t>() <= 16),
                    "requireGeneratedCount must be an exact integer from 0 through 16.");
            const auto finishStep = [&]
            {
                drainGenerated(name);
                if (expectedGenerated != step.end())
                    Require(generated.size() == expectedGenerated->get<std::size_t>(),
                        "The retained host-generated artifact count did not match this step's exact mechanical oracle.");
            };
            if (operation == "attach_data" || operation == "attach_file")
            {
                const auto file = std::filesystem::canonical(step.at("path").get<std::string>());
                const auto bytes = Read(file, 64 * 1024 * 1024);
                const auto attached = operation == "attach_data" ? session.AttachImageData(step.at("displayName").get<std::string>(), bytes)
                                                                 : session.AttachFile(file);
                trace.Emit({{"kind", "intake"}, {"step", name}, {"operation", operation},
                    {"accepted", attached.status == revia::attachments::IntakeStatus::Accepted}, {"message", attached.message},
                    {"id", attached.attachment.id}, {"inputSha256", revia::audit::ContentDigest(bytes)},
                    {"storedSha256", attached.attachment.digest}, {"width", attached.attachment.width},
                    {"mediaKind", revia::attachments::ToString(attached.attachment.kind)}, {"mimeType", attached.attachment.mimeType},
                    {"durationMs", attached.attachment.video.durationMs}, {"videoCodec", attached.attachment.video.videoCodec},
                    {"hasAudio", attached.attachment.video.hasAudio}, {"height", attached.attachment.height},
                    {"elapsedMs", Milliseconds(began)}});
                const bool accepted = attached.status == revia::attachments::IntakeStatus::Accepted;
                Require(
                    accepted == step.value("expectAccepted", true), "Actual image intake did not match the expected admission outcome.");
                if (!accepted)
                {
                    finishStep();
                    continue;
                }
                Require(attached.attachment.digest == revia::audit::ContentDigest(bytes), "The staged image bytes changed unexpectedly.");
                ids.emplace(name, attached.attachment.id);
                storedFiles.emplace(name, attached.attachment.storedPath);
            }
            else if (operation == "submit" || operation == "submit_cancel" || operation == "submit_while_video" ||
                     operation == "regenerate" || operation == "edit")
            {
                if (operation == "submit_while_video")
                {
                    Require(allowVideoGeneration, "Concurrent video/chat qualification requires the explicit video workflow gate.");
                    const int seconds = step.value("videoWaitTimeoutSeconds", 60);
                    Require(seconds > 0 && seconds <= 600, "The wait for an active video phase must be bounded to 600 seconds.");
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                    const auto phase = step.value("videoPhase", std::string{});
                    Require(phase.empty() || phase == "loading" || phase == "generating" || phase == "verifying",
                        "Select an actual active video phase for the chat overlap check.");
                    while (std::chrono::steady_clock::now() < deadline)
                    {
                        session.PollBackgroundEvents();
                        drainGenerated(name);
                        const auto snapshot = session.VideoGenerationSnapshot();
                        if (VideoActive(snapshot) && (phase.empty() || snapshot.state == phase))
                            break;
                        Require(snapshot.state != "failed" && snapshot.state != "cancelled" && snapshot.state != "succeeded",
                            "The video job ended before the requested chat overlap phase.");
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    const auto snapshot = session.VideoGenerationSnapshot();
                    Require(VideoActive(snapshot) && (phase.empty() || snapshot.state == phase),
                        "No active video job was observed before the concurrent ordinary turn.");
                }
                std::vector<std::string> selected;
                for (const auto& alias : step.value("attachments", Json::array()))
                    selected.push_back(ids.at(alias.get<std::string>()));
                const auto input = step.value("input", std::string{});
                const auto mediaEpoch = session.ConversationMediaEpoch();
                const auto videoBefore = session.VideoGenerationSnapshot();
                const auto submitted = std::chrono::steady_clock::now();
                bool retracted = false;
                SessionResult answer;
                bool cancelledAfterRequest = false;
                if (operation == "submit_cancel")
                {
                    Require(!input.empty(), "Cancellation qualification requires an explicit nonempty authored input.");
                    const auto captureRoot = std::filesystem::canonical(workflow.at("captureRoot").get<std::string>());
                    std::set<std::filesystem::path> prior;
                    for (const auto& file : std::filesystem::recursive_directory_iterator(captureRoot))
                        if (file.is_regular_file())
                            prior.insert(file.path());
                    std::atomic<bool> finished = false;
                    std::exception_ptr failure;
                    std::jthread worker(
                        [&]
                        {
                            try
                            {
                                answer = session.SubmitWithAttachments(input, selected);
                            }
                            catch (...)
                            {
                                failure = std::current_exception();
                            }
                            finished = true;
                        });
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
                    while (!finished && std::chrono::steady_clock::now() < deadline && !cancelledAfterRequest)
                    {
                        for (const auto& file : std::filesystem::recursive_directory_iterator(captureRoot))
                        {
                            if (!file.is_regular_file() || prior.contains(file.path()) || file.path().extension() != ".json")
                                continue;
                            try
                            {
                                const auto request = Json::parse(Read(file.path(), 8 * 1024 * 1024));
                                if (request.value("path", "") == "/v1/chat/completions" && request.at("body").contains("messages") &&
                                    request.at("body").at("messages").is_array() && !request.at("body").at("messages").empty() &&
                                    request.at("body").dump().find(input) != std::string::npos)
                                {
                                    session.RequestStop();
                                    cancelledAfterRequest = true;
                                    break;
                                }
                            }
                            catch (...)
                            {
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if (!finished)
                        session.RequestStop();
                    worker.join();
                    if (failure)
                        std::rethrow_exception(failure);
                    Require(cancelledAfterRequest, "The cancellation case did not observe an actual conversation request before stopping.");
                }
                else
                    answer = operation == "regenerate" ? session.RegenerateLast(retracted)
                             : operation == "edit"     ? session.ReplaceLastUserTurn(input, retracted)
                                                       : session.SubmitWithAttachments(input, selected);
                ++turns;
                const double submitElapsed = Milliseconds(submitted);
                trace.Emit({{"kind", "reply"}, {"step", name}, {"operation", operation}, {"input", input}, {"attachmentIds", selected},
                    {"succeeded", answer.succeeded}, {"finalReply", answer.text}, {"reason", answer.reason}, {"trace", answer.reasoning},
                    {"elapsedMs", Milliseconds(began)}, {"semanticVerdict", "pending independent review"}, {"exchangeRetracted", retracted},
                    {"cancelledAfterActualRequest", cancelledAfterRequest}, {"resources", session.ResourceUsage().Summary()},
                    {"generatedMediaReferences", References(answer.generatedMediaReferences)}, {"submitElapsedMs", submitElapsed},
                    {"videoStateBefore", videoBefore.state}, {"videoStepBefore", videoBefore.step},
                    {"videoStateAfter", session.VideoGenerationSnapshot().state}});
                for (const auto& reference : answer.generatedMediaReferences)
                    captureGenerated(name, reference, answer.stamp, answer.audienceRevision, mediaEpoch);
                drainGenerated(name);
                if (step.value("requireSuccess", true))
                    Require(
                        answer.succeeded && !answer.text.empty(), "NOT VERIFIED: an actual session image turn failed or returned no text.");
                if (operation == "submit_cancel")
                    Require(!answer.succeeded, "The controlled image turn completed successfully before cancellation could retire it.");
                if ((operation == "regenerate" || operation == "edit") && step.value("requireRetraction", true))
                    Require(retracted, "NOT VERIFIED: the requested replay did not retract the prior exchange.");
                const double maximum = step.value("maxElapsedMs", 0.0);
                Require(maximum <= 0 || submitElapsed <= maximum, "The ordinary turn exceeded its explicit latency bound.");
            }
            else if (operation == "wait_video")
            {
                Require(allowVideoGeneration, "Waiting for generated video requires the explicit workflow gate.");
                const int seconds = step.value("timeoutSeconds", 600);
                Require(seconds > 0 && seconds <= 600, "A generated-video wait must be bounded to 600 seconds.");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                std::string priorState;
                while (std::chrono::steady_clock::now() < deadline && generatedVideos <= waitedVideos)
                {
                    session.PollBackgroundEvents();
                    drainGenerated(name);
                    const auto snapshot = session.VideoGenerationSnapshot();
                    if (snapshot.state != priorState)
                    {
                        trace.Emit({{"kind", "video_status"}, {"step", name}, {"state", snapshot.state}, {"jobId", snapshot.jobId},
                            {"videoStep", snapshot.step}, {"steps", snapshot.steps}, {"detail", snapshot.detail},
                            {"elapsedMs", Milliseconds(began)}, {"resources", session.ResourceUsage().Summary()}});
                        priorState = snapshot.state;
                    }
                    if (snapshot.state == "failed" || snapshot.state == "cancelled")
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                const bool completed = generatedVideos > waitedVideos;
                const auto finalState = session.VideoGenerationSnapshot().state;
                trace.Emit({{"kind", "video_wait"}, {"step", name}, {"completed", completed}, {"elapsedMs", Milliseconds(began)},
                    {"state", finalState}, {"generatedVideos", generatedVideos}});
                Require(completed || finalState == "failed" || finalState == "cancelled",
                    "The video wait expired without a host artifact or an explicit terminal failure.");
                Require(
                    completed == step.value("requireSuccess", true), "The video wait did not match the expected host artifact outcome.");
                if (completed)
                {
                    ids[name] = ids.at("latest_generated_video");
                    storedFiles[name] = storedFiles.at("latest_generated_video");
                }
                waitedVideos = generatedVideos;
            }
            else if (operation == "forget")
            {
                std::string error;
                const bool forgotten = session.ForgetDocument(ids.at(step.at("attachment").get<std::string>()), error);
                trace.Emit({{"kind", "forget"}, {"step", name}, {"succeeded", forgotten}, {"reason", error}});
                Require(forgotten, "The explicitly selected disposable image could not be forgotten.");
            }
            else if (operation == "remember_conversation")
            {
                const auto id = session.CurrentConversationId();
                Require(!id.empty(), "The enabled archive did not supply a current conversation id.");
                conversations.emplace(name, id);
                trace.Emit({{"kind", "conversation_receipt"}, {"step", name}, {"id", id}});
            }
            else if (operation == "new_conversation" || operation == "restore_conversation")
            {
                std::string error;
                const bool changed =
                    operation == "new_conversation"
                        ? session.StartNewConversation(error)
                        : session.ContinueConversation(conversations.at(step.at("conversation").get<std::string>()), error);
                trace.Emit({{"kind", "conversation_change"}, {"step", name}, {"operation", operation}, {"succeeded", changed},
                    {"reason", error}, {"currentId", session.CurrentConversationId()}});
                Require(changed, "NOT VERIFIED: the requested conversation operation was refused.");
            }
            else if (operation == "tamper_stored" || operation == "remove_stored")
            {
                const auto ownedFile = std::filesystem::canonical(storedFiles.at(step.at("attachment").get<std::string>()));
                const auto relative = ownedFile.lexically_relative(root);
                Require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
                    "The diagnostic mutation is confined to this fresh runtime's own private image copy.");
                if (operation == "remove_stored")
                {
                    Require(std::filesystem::remove(ownedFile), "The owned diagnostic image copy could not be removed.");
                    trace.Emit({{"kind", "diagnostic_mutation"}, {"step", name}, {"operation", operation}});
                    finishStep();
                    continue;
                }
                const auto replacement = Read(std::filesystem::canonical(step.at("replacement").get<std::string>()), 8 * 1024 * 1024);
                std::ofstream output(ownedFile, std::ios::binary | std::ios::trunc);
                output.write(replacement.data(), static_cast<std::streamsize>(replacement.size()));
                Require(output.good(), "The diagnostic mutation could not be written to its owned scratch file.");
                trace.Emit({{"kind", "diagnostic_mutation"}, {"step", name}, {"operation", operation},
                    {"replacementSha256", revia::audit::ContentDigest(replacement)}});
            }
            else if (operation == "audience")
            {
                auto audience = session.Audience();
                const auto kind = step.at("kind").get<std::string>();
                Require(kind == "private" || kind == "public", "Select private or public audience for the diagnostic workflow.");
                audience.kind = kind == "private" ? revia::identity::AudienceKind::Private : revia::identity::AudienceKind::Public;
                std::string error;
                const bool changed = session.SetAudience(audience, error);
                trace.Emit({{"kind", "audience_change"}, {"step", name}, {"audience", kind}, {"succeeded", changed}, {"reason", error}});
                Require(changed, "The requested disposable audience transition was refused.");
            }
            else
                throw std::runtime_error("The workflow names an unsupported operation: " + operation);
            finishStep();
        }
        session.PollBackgroundEvents();
        drainGenerated("workflow_completion");
        if (workflow.value("persistDriverState", false))
        {
            const auto statePath = root / "vision-driver-state.json";
            Require(!std::filesystem::exists(statePath), "Existing driver state is never replaced.");
            Json files = Json::object();
            for (const auto& [alias, path] : storedFiles)
                files[alias] = path.string();
            std::ofstream output(statePath, std::ios::binary);
            output << Json{{"ids", ids}, {"conversations", conversations}, {"storedFiles", files}}.dump(2);
            Require(output.good(), "The explicit driver continuation state could not be retained.");
        }
        session.Stop();
        trace.Emit({{"kind", "complete"}, {"turns", turns}, {"transportVerified", expectVision}, {"semanticQualified", false},
            {"generatedArtifacts", generated.size()}, {"generatedVideos", generatedVideos},
            {"qualification", "Actual payload and content correctness require independent retained-traffic review."}});
        return 0;
    }
    catch (const std::exception& error)
    {
        if (trace.output.is_open() && trace.output.good())
            trace.Emit({{"kind", "error"}, {"reason", error.what()}, {"semanticQualified", false}});
        std::cerr << error.what() << '\n';
        return 2;
    }
}
