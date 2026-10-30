#include "Audit/contentDigest.h"
#include "Core/configManager.h"
#include "Runtime/reviaSession.h"

#include <algorithm>
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
#include <vector>

namespace
{
using Json = nlohmann::json;
using namespace revia::runtime;

void Require(const bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}

std::string Read(const std::filesystem::path& path, const std::size_t limit)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "The explicitly selected file could not be read.");
    std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    Require(input.eof() && !input.bad() && bytes.size() <= limit, "The selected file exceeded its read bound.");
    return bytes;
}

double Milliseconds(const std::chrono::steady_clock::time_point began)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
}

struct Trace
{
    std::ofstream output;
    std::mutex mutex;

    void Emit(Json value)
    {
        std::lock_guard lock(mutex);
        value["recordedAtUnixMs"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        output << value.dump() << '\n' << std::flush;
        Require(output.good(), "The new evidence report could not be written.");
    }
};

struct WorkingDirectory
{
    std::filesystem::path before = std::filesystem::current_path();
    explicit WorkingDirectory(const std::filesystem::path& path)
    {
        std::filesystem::current_path(path);
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
        {"width", reference.width}, {"height", reference.height},
        {"origin", reference.origin == MediaOrigin::GeneratedArtifact ? "generated" : "upload"}};
}

Json References(const std::vector<conversationMediaReference>& references)
{
    auto result = Json::array();
    for (const auto& reference : references)
        result.push_back(Reference(reference));
    return result;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: ReviaCompanionImageLive <fresh-runtime-directory> <new-report.jsonl> <reference.png>\n";
        return 2;
    }
    Trace trace;
    try
    {
        const auto root = std::filesystem::canonical(argv[1]);
        const auto report = std::filesystem::absolute(argv[2]);
        const auto reference = std::filesystem::canonical(argv[3]);
        Require(!std::filesystem::exists(report), "Existing evidence is never replaced.");
        trace.output.open(report, std::ios::binary);
        Require(trace.output.good(), "The new report could not be opened.");
        for (const char* path : {"RuntimeData", "Memory", "Identity", "Logs"})
            Require(!std::filesystem::exists(root / path), "Use a fresh isolated runtime.");
        WorkingDirectory cwd(root);
        configManager config((root / "Config/settings.json").string(), (root / "Config/Profiles").string());
        appSettings settings;
        aiProfile profile;
        Require(config.LoadSettings(settings) && config.LoadProfile(settings.activeProfile, profile),
            "Disposable settings/profile did not load.");
        Require(settings.image.bEnabled && settings.image.host == "127.0.0.1" && settings.image.bOffline &&
                    settings.image.bShutdownOnExit && !settings.image.bKeepLoaded && !settings.llm.bAutoStartServer &&
                    !settings.llm.bVisionEnabled && !settings.llm.cloud.bEnabled && !settings.intelligence.bEnabled &&
                    !settings.embedding.bEnabled && !settings.speech.bEnabled && !settings.speechRecognition.bEnabled &&
                    !settings.vision.bEnabled && !settings.perception.bEnabled && !settings.initiative.bEnabled &&
                    !settings.localApi.bEnabled && !settings.performance.bEnabled && !settings.improvement.bEnabled &&
                    !settings.presence.bEnabled && !settings.video.bEnabled && !profile.bMemoryEnabled && settings.mcp.servers.empty(),
            "Configure the existing offline image provider only, with chat/vision and all auxiliary workers disabled.");
        for (const int port : {settings.llm.port, settings.image.port})
        {
            httplib::Client existing("127.0.0.1", port);
            existing.set_connection_timeout(1);
            existing.set_read_timeout(1);
            Require(!existing.Get("/health"), "A selected port already has a responder; no foreign service will be driven.");
        }
        trace.Emit({{"kind", "preflight"}, {"provider", settings.image.model}, {"offline", settings.image.bOffline},
            {"width", settings.image.width}, {"height", settings.image.height}, {"steps", settings.image.steps},
            {"profileSha256", revia::audit::ContentDigest(Read(root / "Config/Profiles" / (settings.activeProfile + ".json"), 1048576))}});
        std::mutex publicationMutex;
        std::vector<RuntimeEvent> pendingPublications;
        bool publicationOverflow = false;
        ReviaSession session;
        StopSession stop{session};
        session.Events().Subscribe(
            [&](const RuntimeEvent& event)
            {
                if (event.component == "Image" || event.component == "Canvas")
                {
                    trace.Emit({{"kind", "native_event"}, {"component", event.component}, {"phase", event.phase}, {"text", event.message},
                        {"detail", event.detail}, {"elapsedMs", event.elapsedMilliseconds},
                        {"generatedMediaReference",
                            event.generatedMediaReference ? Reference(*event.generatedMediaReference) : Json(nullptr)}});
                    if (event.component == "Canvas" && event.phase == "Image")
                    {
                        std::lock_guard lock(publicationMutex);
                        if (pendingPublications.size() < 32)
                            pendingPublications.push_back(event);
                        else
                            publicationOverflow = true;
                    }
                }
            });
        const auto started = std::chrono::steady_clock::now();
        Require(session.Start(), "The isolated native image session did not start.");
        trace.Emit({{"kind", "session_start"}, {"elapsedMs", Milliseconds(started)}, {"resources", session.ResourceUsage().Summary()}});
        const std::string prompt = "A watercolor illustration of a red fox with a blue scarf under a snowy pine tree.";
        session.SetConfirmationHandler(
            [](const revia::actions::ActionRequest& request, const revia::actions::PolicyDecision&)
            {
                return request.type == revia::actions::ActionType::GenerateImage ? revia::actions::ConfirmationChoice::Allow
                                                                                 : revia::actions::ConfirmationChoice::Decline;
            });
        std::vector<revia::attachments::StoredDocument> published;
        std::map<std::string, conversationMediaReference> generated;
        std::set<std::string> canvasIds;
        const auto capture = [&](const conversationMediaReference& source, const RuntimeStamp& stamp, const std::uint64_t audienceRevision,
                                 const std::uint64_t mediaEpoch)
        {
            Require(source.origin == MediaOrigin::GeneratedArtifact && source.mimeType == "image/png" && !stamp.companionId.empty() &&
                        stamp.SameSession(session.Stamp()) && stamp.policyVersion == session.Stamp().policyVersion &&
                        audienceRevision != 0 && audienceRevision == session.Audience().revision &&
                        session.Audience().kind == revia::identity::AudienceKind::Private && mediaEpoch == session.ConversationMediaEpoch(),
                "The generated image was not published with current private host source admission.");
            const auto documents = session.Documents();
            const auto document = std::find_if(documents.begin(), documents.end(),
                [&](const auto& item)
                {
                    return item.id == source.id && item.digest == source.digest && item.mimeType == source.mimeType &&
                           item.origin == MediaOrigin::GeneratedArtifact && item.generated.has_value();
                });
            Require(document != documents.end(), "The generated image has no matching canonical host library record.");
            const auto path = std::filesystem::canonical(document->storedPath);
            const auto relative = path.lexically_relative(root);
            Require(!relative.empty() && !relative.is_absolute() && *relative.begin() != ".." &&
                        !std::filesystem::is_symlink(std::filesystem::symlink_status(document->storedPath)),
                "The generated image is not an owned canonical file inside the disposable runtime.");
            const auto bytes = Read(path, 64 * 1024 * 1024);
            Require(bytes.size() == document->bytes && revia::audit::ContentDigest(bytes) == source.digest,
                "Canonical generated pixels no longer match the host digest.");
            const auto found = generated.find(source.id);
            if (found != generated.end())
            {
                Require(found->second == source, "A generated source ID changed metadata between result and Canvas publication.");
                return;
            }
            generated.emplace(source.id, source);
            published.push_back(*document);
        };
        const auto record = [&](const std::string& step, const SessionResult& result, const double elapsed)
        {
            std::vector<RuntimeEvent> events;
            {
                std::lock_guard lock(publicationMutex);
                Require(!publicationOverflow, "Canvas publication retention exceeded its fixed bound.");
                events.swap(pendingPublications);
            }
            for (const auto& event : events)
            {
                Require(event.generatedMediaReference.has_value(), "Canvas published an image without a typed canonical source.");
                capture(*event.generatedMediaReference, event.stamp, event.audienceRevision, event.conversationMediaEpoch);
                canvasIds.insert(event.generatedMediaReference->id);
            }
            for (const auto& source : result.generatedMediaReferences)
                capture(source, result.stamp, result.audienceRevision, session.ConversationMediaEpoch());
            trace.Emit({{"kind", "reply"}, {"step", step}, {"succeeded", result.succeeded}, {"finalReply", result.text},
                {"reason", result.reason}, {"trace", result.reasoning}, {"elapsedMs", elapsed}, {"canvasPublications", canvasIds.size()},
                {"generatedArtifactCount", published.size()}, {"generatedMediaReferences", References(result.generatedMediaReferences)},
                {"resources", session.ResourceUsage().Summary()}});
        };
        auto began = std::chrono::steady_clock::now();
        const auto first = session.GenerateImage(prompt);
        record("native_generate", first, Milliseconds(began));
        Require(first.succeeded && published.size() == 1 && canvasIds.size() == 1 && first.generatedMediaReferences.size() == 1,
            "Native GenerateImage did not publish one actual host-associated image.");
        std::stop_source cancellation;
        cancellation.request_stop();
        began = std::chrono::steady_clock::now();
        const auto cancelled = session.GenerateImage(prompt, cancellation.get_token());
        record("cancelled_before_effect", cancelled, Milliseconds(began));
        Require(!cancelled.succeeded && published.size() == 1 && canvasIds.size() == 1 && cancelled.generatedMediaReferences.empty(),
            "A cancelled native image request published an artifact.");
        began = std::chrono::steady_clock::now();
        const auto repeat = session.Submit("Create a picture of a red fox wearing a blue scarf under a snowy pine tree.");
        record("native_drawing_dispatch", repeat, Milliseconds(began));
        Require(repeat.succeeded && published.size() == 2 && canvasIds.size() == 2 && repeat.generatedMediaReferences.size() == 1,
            "Natural drawing dispatch did not publish one additional actual image.");
        const auto attached = session.AttachFile(reference);
        Require(attached.status == revia::attachments::IntakeStatus::Accepted,
            "The explicit reference-edit control could not stage its image.");
        began = std::chrono::steady_clock::now();
        const auto edit = session.SubmitWithAttachments(
            "Edit this image into a reference-guided variation with warmer orange colors.", {attached.attachment.id});
        record("reference_guided_variation", edit, Milliseconds(began));
        Require(edit.succeeded && !edit.text.empty() && published.size() == 3 && canvasIds.size() == 3 &&
                    edit.generatedMediaReferences.size() == 1 && published.back().generated &&
                    published.back().generated->mode == "reference_to_image" &&
                    published.back().generated->sourceId == attached.attachment.id &&
                    published.back().generated->sourceDigest == attached.attachment.digest,
            "Reference-guided generation did not publish an actual variation bound to the accepted source ID/digest.");
        began = std::chrono::steady_clock::now();
        const auto fresh = session.Submit("Draw a new picture of a calm fox resting beside a pond.");
        record("fresh_drawing_after_reference", fresh, Milliseconds(began));
        Require(fresh.succeeded && published.size() == 4 && canvasIds.size() == 4 && fresh.generatedMediaReferences.size() == 1 &&
                    published.back().generated && published.back().generated->mode == "text_to_image" &&
                    published.back().generated->sourceId.empty() && published.back().generated->sourceDigest.empty(),
            "A clearly fresh drawing request after reference-guided generation did not publish a fourth standalone image.");
        const auto evidenceDirectory = report.parent_path() / (report.stem().string() + "-generated-artifacts");
        Require(!std::filesystem::exists(evidenceDirectory) && std::filesystem::create_directory(evidenceDirectory),
            "Use a fresh generated-artifact evidence directory.");
        std::size_t sequence = 0;
        for (const auto& document : published)
        {
            const auto bytes = Read(document.storedPath, 64 * 1024 * 1024);
            Require(revia::audit::ContentDigest(bytes) == document.digest, "A generated artifact changed before evidence retention.");
            const auto copy = evidenceDirectory / (std::to_string(++sequence) + "-" + document.digest + ".png");
            Require(!std::filesystem::exists(copy), "Existing generated-artifact evidence is never replaced.");
            std::ofstream output(copy, std::ios::binary);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            Require(output.good() && revia::audit::ContentDigest(Read(copy, 64 * 1024 * 1024)) == document.digest,
                "The actual generated image could not be retained byte-for-byte.");
            const auto& provenance = *document.generated;
            trace.Emit({{"kind", "artifact"}, {"id", document.id}, {"canonicalStoredPath", document.storedPath.string()},
                {"evidencePath", copy.string()}, {"bytes", bytes.size()}, {"sha256", document.digest}, {"mode", provenance.mode},
                {"sourceId", provenance.sourceId}, {"sourceDigest", provenance.sourceDigest}, {"seed", provenance.seed},
                {"settingsDigest", provenance.settingsDigest}, {"visualQuality", "pending independent image review"}});
        }
        session.Stop();
        trace.Emit({{"kind", "complete"}, {"nativeGenerationObserved", true}, {"actualCanvasArtifacts", canvasIds.size()},
            {"referenceGuidedSourceBindingVerified", true}, {"artisticQualityQualified", false}, {"combinedVisionLoadQualified", false}});
        return 0;
    }
    catch (const std::exception& error)
    {
        if (trace.output.is_open() && trace.output.good())
            trace.Emit({{"kind", "error"}, {"reason", error.what()}});
        std::cerr << error.what() << '\n';
        return 2;
    }
}
