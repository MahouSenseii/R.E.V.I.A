#include "Actions/actionTypes.h"
#include "Attachments/attachmentLibrary.h"
#include "Attachments/videoSelection.h"
#include "Audit/contentDigest.h"
#include "Core/runtimePath.h"
#include "Core/utf8.h"
#include "Memory/conversationArchive.h"
#include "Process/ownedProcess.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Json = nlohmann::json;
using namespace revia::attachments;

void Require(const bool condition, const std::string& reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

std::string Read(const std::filesystem::path& path, const std::uintmax_t limit = 64U * 1024U * 1024U)
{
    const auto size = std::filesystem::file_size(path);
    Require(size <= limit, "An evidence input exceeded its bounded read limit.");
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(size));
    Require(static_cast<std::uintmax_t>(input.gcount()) == size && input.peek() == std::char_traits<char>::eof(),
        "An evidence input was incomplete or changed while being read.");
    return bytes;
}

void WriteNew(const std::filesystem::path& path, const std::string& bytes)
{
    Require(!std::filesystem::exists(path), "Refusing to overwrite an existing evidence file.");
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Require(static_cast<bool>(output), "An evidence file could not be written.");
}

Json Reference(const conversationMediaReference& reference)
{
    return {{"id", reference.id}, {"digest", reference.digest}, {"name", reference.name}, {"mimeType", reference.mimeType},
        {"width", reference.width}, {"height", reference.height}, {"durationMs", reference.durationMs},
        {"sampleStartMs", reference.sampleStartMs}, {"sampleEndMs", reference.sampleEndMs}};
}

struct Evidence
{
    std::filesystem::path root;
    bool created = false;
    Json receipt{{"schemaVersion", 1}, {"kind", "stage-c-video-store-real-tool-acceptance"}, {"status", "running"},
        {"modelOrWhisperStarted", false}, {"semanticModelReview", "pending"}, {"checks", Json::array()}};

    void Check(const std::string& name, const bool passed, Json details = Json::object())
    {
        Json row{{"name", name}, {"passed", passed}, {"details", std::move(details)}};
        receipt["checks"].push_back(row);
        std::ofstream journal(root / "checks.jsonl", std::ios::app | std::ios::binary);
        journal << row.dump() << '\n';
        journal.flush();
        Require(static_cast<bool>(journal), "The acceptance journal could not be persisted.");
        Require(passed, "Acceptance failed: " + name);
    }

    void Finish(const bool passed, const std::string& error = {})
    {
        receipt["status"] = passed ? "mechanical-checks-passed" : "failed";
        receipt["error"] = error;
        WriteNew(root / "receipt.json", receipt.dump(2) + "\n");
    }
};

std::string DecodeRgb(const std::filesystem::path& png, const std::uint32_t width, const std::uint32_t height)
{
    const auto ffmpeg = revia::core::ResolveRuntimePath(std::filesystem::path("ThirdParty/ffmpeg/bin/ffmpeg.exe"));
    revia::process::OwnedProcess process;
    revia::process::OwnedProcessRequest request;
    request.executable = ffmpeg;
    request.arguments = {"-nostdin", "-hide_banner", "-v", "error", "-protocol_whitelist", "file", "-threads", "1", "-i",
        revia::actions::PathToUtf8(png), "-frames:v", "1", "-threads", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"};
    const auto expected = static_cast<std::size_t>(width) * height * 3;
    Require(expected > 0 && expected <= 4U * 1024U * 1024U, "PNG dimensions exceed the independent pixel-check budget.");
    request.maxPendingOutputBytes = expected + 1;
    std::string error;
    Require(process.Start(request, error), "The independent PNG decoder could not start: " + error);
    std::string rgb;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline)
    {
        rgb += process.Read(std::chrono::milliseconds(10));
        Require(rgb.size() <= expected, "The PNG decoder exceeded its expected output length.");
        if (const auto code = process.ExitCode(); code && !process.Running())
        {
            Require(*code == 0 && process.StopReason().empty() && rgb.size() == expected,
                "A retained PNG did not independently decode to its exact RGB dimensions.");
            return rgb;
        }
    }
    throw std::runtime_error("Independent PNG decoding exceeded its time limit.");
}

std::array<int, 3> Pixel(const std::string& rgb, const std::uint32_t width, const int x, const int y)
{
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 3;
    Require(x >= 0 && y >= 0 && static_cast<std::uint32_t>(x) < width && offset + 2 < rgb.size(), "Pixel probe is outside the image.");
    return {
        static_cast<unsigned char>(rgb[offset]), static_cast<unsigned char>(rgb[offset + 1]), static_cast<unsigned char>(rgb[offset + 2])};
}

std::int64_t FrameTimestamp(const std::string& name)
{
    std::smatch match;
    const std::regex timestamp(R"(^Video frame at ([0-9]{2}):([0-9]{2})\.([0-9]{3})$)");
    Require(std::regex_match(name, match, timestamp), "A sampled image lost its explicit frame timestamp.");
    return std::stoll(match[1].str()) * 60000 + std::stoll(match[2].str()) * 1000 + std::stoll(match[3].str());
}

Json SaveFrame(Evidence& evidence, const std::string& relative, const conversationImage& image)
{
    Require(image.bytes && !image.bytes->empty() && image.mimeType == "image/png", "A frame has no actual PNG payload.");
    WriteNew(evidence.root / relative, *image.bytes);
    return {{"path", relative}, {"sha256", revia::audit::ContentDigest(*image.bytes)}, {"bytes", image.bytes->size()}, {"name", image.name},
        {"width", image.width}, {"height", image.height}};
}

Json CheckFrames(Evidence& evidence, const std::string& phase, const TurnAttachments& turn, const std::vector<std::int64_t>& expectedTimes,
    const std::filesystem::path& fixtures, const Json& manifest)
{
    evidence.Check(phase + "-frame-budget",
        turn.images.size() == expectedTimes.size() && turn.images.size() <= 4 && turn.mediaReferences.size() == 1 &&
            turn.visibleMediaReferences == turn.mediaReferences && !turn.transcriptAvailable,
        {{"frames", turn.images.size()}, {"references", turn.mediaReferences.size()}, {"notices", turn.notices}});
    Json frames = Json::array();
    std::size_t redCount = 0;
    for (std::size_t index = 0; index < turn.images.size(); ++index)
    {
        const auto& image = turn.images[index];
        const auto time = FrameTimestamp(image.name);
        const auto relative = phase + "-frame-" + std::to_string(index + 1) + ".png";
        Json frame = SaveFrame(evidence, relative, image);
        frame["timestampMs"] = time;
        const auto rgb = DecodeRgb(evidence.root / relative, image.width, image.height);
        const auto center = Pixel(rgb, image.width, 320, 160);
        const bool red = center[0] > 180 && center[1] < 70 && center[2] < 80;
        frame["centerRgb"] = center;
        frame["centerRedSquare"] = red;
        redCount += red;
        std::string sourceId;
        for (const auto& interval : manifest.at("groundTruthIntervals"))
            if (time >= static_cast<std::int64_t>(interval.at("startSeconds").get<double>() * 1000) &&
                time < static_cast<std::int64_t>(interval.at("endSeconds").get<double>() * 1000))
                sourceId = interval.at("sourceId").get<std::string>();
        const auto source = std::find_if(manifest.at("sourceImages").begin(), manifest.at("sourceImages").end(),
            [&](const auto& item) { return item.at("id").template get<std::string>() == sourceId; });
        Require(source != manifest.at("sourceImages").end(), "The frame time has no independent source-image oracle.");
        Require(image.width == 640 && image.height == 360, "The known fixture's dimensions changed unexpectedly.");
        const auto expectedRgb = DecodeRgb(fixtures / source->at("path").get<std::string>(), 640, 360);
        int maximumError = 0;
        for (const auto& point : std::array<std::array<int, 2>, 4>{{{150, 150}, {490, 150}, {320, 160}, {320, 40}}})
        {
            const auto actual = Pixel(rgb, 640, point[0], point[1]);
            const auto expected = Pixel(expectedRgb, 640, point[0], point[1]);
            for (std::size_t channel = 0; channel < 3; ++channel)
                maximumError = std::max(maximumError, std::abs(actual[channel] - expected[channel]));
        }
        const bool expectedRed = time >= 12000 && time < 12500;
        frame["oracleSourceId"] = sourceId;
        frame["maxColorProbeChannelError"] = maximumError;
        frames.push_back(frame);
        evidence.Check(
            phase + "-frame-" + std::to_string(index + 1), time == expectedTimes[index] && red == expectedRed && maximumError <= 12, frame);
    }
    evidence.Check(phase + "-sparse-event-coverage", redCount == (phase == "interval" ? 1u : 0u),
        {{"redFrames", redCount}, {"visualModelJudgment", "pending"}, {"reference", turn.reference}});
    return frames;
}

void CheckPendingTranscriptionCancellation(Evidence& evidence, const std::filesystem::path& fixtures, const bool forget)
{
    const std::string phase = forget ? "forget-inflight-transcription" : "caller-stop-inflight-transcription";
    const auto root = evidence.root / phase;
    Require(std::filesystem::create_directory(root), "The cancellation control directory could not be created.");
    AttachmentLibrary library(root / "Files", root / "library.db", root / "audit.jsonl");
    const auto inputDigest = revia::audit::ContentDigest(Read(fixtures / "clip-03.mp4"));
    const auto staged = library.Stage(fixtures / "clip-03.mp4");
    Require(staged.status == IntakeStatus::Accepted && staged.attachment.video.hasAudio,
        "The real audio-bearing cancellation fixture could not be staged.");
    std::promise<void> entered;
    auto started = entered.get_future();
    std::atomic<bool> observedStop{false};
    std::atomic<bool> stoppable{false};
    std::atomic<std::int64_t> callbackMs{0};
    std::filesystem::path temporaryWave;
    library.SetVideoTranscriber(
        [&](const std::filesystem::path& wave, const std::stop_token token)
        {
            const auto beginning = std::chrono::steady_clock::now();
            temporaryWave = wave;
            stoppable.store(token.stop_possible());
            entered.set_value();
            while (!token.stop_requested() && std::chrono::steady_clock::now() - beginning < std::chrono::seconds(2))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            observedStop.store(token.stop_requested());
            callbackMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - beginning).count());
            revia::speech::FileTranscriptionResult result;
            result.ok = true;
            result.segments.push_back({0, 500, "Synthetic callback evidence must not survive cancellation."});
            return result;
        });
    std::stop_source callerStop;
    auto preparation = std::async(std::launch::async,
        [&]
        {
            return library.PrepareTurn("Transcribe the speech in this clip.", library.TakeStaged({staged.attachment.id}),
                {false, "Vision is not used by this cancellation control."}, callerStop.get_token());
        });
    if (started.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    {
        callerStop.request_stop();
        preparation.wait();
        throw std::runtime_error("The cancellation control transcriber did not start within its decode allowance.");
    }
    std::string error;
    const bool withdrawn = forget ? library.Forget(staged.attachment.id, error) : callerStop.request_stop();
    const auto turn = preparation.get();
    const bool evidenceCleared = turn.images.empty() && turn.visibleMediaReferences.empty() && turn.videoTimestampsMs.empty() &&
                                 !turn.transcriptAvailable && turn.videoTranscript.empty() &&
                                 turn.reference.find("Synthetic callback evidence") == std::string::npos;
    const bool temporaryRemoved = !std::filesystem::exists(temporaryWave);
    if (!forget)
        Require(library.Forget(staged.attachment.id, error), "The caller-stop control source could not be removed: " + error);
    evidence.Check(phase + "-cancels-owned-work",
        withdrawn && stoppable.load() && observedStop.load() && callbackMs.load() <= 500 && evidenceCleared && temporaryRemoved &&
            !std::filesystem::exists(staged.attachment.storedPath) && library.List().empty() && library.Staged().empty() &&
            std::filesystem::is_empty(root / "Files") && revia::audit::ContentDigest(Read(fixtures / "clip-03.mp4")) == inputDigest,
        {{"callbackObservedStop", observedStop.load()}, {"callbackMs", callbackMs.load()}, {"evidenceCleared", evidenceCleared},
            {"temporaryPcmRemoved", temporaryRemoved}, {"forgetError", error}, {"provider", "bounded fake transcriber; no ASR model"}});
}

void CheckNativeTranscriptDelivery(Evidence& evidence, const std::filesystem::path& fixtures)
{
    const auto root = evidence.root / "native-transcript";
    Require(std::filesystem::create_directory(root), "The native transcript control directory could not be created.");
    AttachmentLibrary library(root / "Files", root / "library.db", root / "audit.jsonl");
    const auto staged = library.Stage(fixtures / "clip-04.mp4");
    Require(staged.status == IntakeStatus::Accepted, "The native transcript fixture could not be staged.");
    std::vector<revia::speech::TranscriptSegment> segments = {{160, 3330, "Courts 19. The final word is meadow."}};
    bool recognitionSucceeded = true;
    std::size_t calls = 0;
    const auto transcribe = [&](const std::filesystem::path&, std::stop_token)
    {
        ++calls;
        revia::speech::FileTranscriptionResult result;
        result.ok = recognitionSucceeded;
        result.error = recognitionSucceeded ? "" : "Synthetic provider unavailable.";
        result.segments = segments;
        return result;
    };
    library.SetVideoTranscriber(transcribe);
    const auto turn = library.PrepareTurn("Transcribe the speech at 00:01.000 to 00:06.000 in the attached clip with timestamps.",
        library.TakeStaged({staged.attachment.id}), {true, {}});
    evidence.Check("pure-transcription-does-not-sample-pixels",
        turn.transcriptAvailable && turn.images.empty() && turn.videoTimestampsMs.empty() && turn.mediaReferences.size() == 1 &&
            turn.visibleMediaReferences == turn.mediaReferences,
        {{"images", turn.images.size()}, {"transcriptAvailable", turn.transcriptAvailable},
            {"provider", "synthetic returned ASR segments; no ASR model"}});
    const auto references = turn.mediaReferences;
    const std::string exact = "[00:01.160–00:04.330] \"Courts 19. The final word is meadow.\"";
    evidence.Check("native-transcript-preserves-actual-words-and-offsets",
        turn.videoTranscript.find(exact) != std::string::npos && turn.videoTranscript.find("Uncertain ASR") != std::string::npos &&
            turn.videoTranscript.find("Quartz") == std::string::npos && turn.videoTranscript.find("final world") == std::string::npos &&
            turn.videoTranscript.size() <= 16384 && references.front().id == staged.attachment.id &&
            references.front().sampleStartMs == 1000 && references.front().sampleEndMs == 6000 &&
            library.ImageReferencesCurrent(references),
        {{"nativeTranscript", turn.videoTranscript}, {"source", Reference(references.front())}});
    const auto noVision = library.ReinspectMedia(
        "Transcribe the speech at 00:01.000 to 00:06.000 in that clip.", references, {false, "Vision intentionally disabled."});
    evidence.Check("native-transcript-does-not-require-vision",
        noVision.transcriptAvailable && noVision.videoTranscript == turn.videoTranscript && noVision.images.empty() &&
            noVision.videoTimestampsMs.empty() && noVision.visibleMediaReferences == noVision.mediaReferences);
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto callsBeforeCancel = calls;
    const auto stopped = library.ReinspectMedia("Transcribe the speech in that clip.", references, {true, {}}, cancelled.get_token());
    evidence.Check("already-cancelled-transcript-releases-no-data",
        stopped.videoTranscript.empty() && !stopped.transcriptAvailable && stopped.images.empty() && calls == callsBeforeCancel);
    segments.clear();
    const auto empty = library.ReinspectMedia("Transcribe any clear speech in that clip.", references, {true, {}});
    evidence.Check("empty-asr-result-is-recognition-not-silence",
        empty.transcriptAvailable && empty.images.empty() &&
            empty.videoTranscript.find("No clear speech was recognized") != std::string::npos &&
            empty.videoTranscript.find("silence") == std::string::npos && empty.videoTranscript.find("silent") == std::string::npos,
        {{"nativeTranscript", empty.videoTranscript}});
    recognitionSucceeded = false;
    const auto failed = library.ReinspectMedia("Transcribe the speech in that clip.", references, {true, {}});
    evidence.Check("failed-asr-produces-no-native-transcript-or-pixels",
        !failed.transcriptAvailable && failed.videoTranscript.empty() && failed.images.empty() && failed.visibleMediaReferences.empty() &&
            std::any_of(failed.notices.begin(), failed.notices.end(),
                [](const auto& notice) { return notice.find("Synthetic provider unavailable") != std::string::npos; }));
    library.SetVideoTranscriber({});
    const auto callsBeforeMissing = calls;
    const auto missing = library.ReinspectMedia("Transcribe the speech in that clip.", references, {true, {}});
    evidence.Check("missing-transcriber-produces-native-unavailable-data", !missing.transcriptAvailable &&
                                                                               missing.videoTranscript.empty() && missing.images.empty() &&
                                                                               calls == callsBeforeMissing && !missing.notices.empty());
    library.SetVideoTranscriber(transcribe);
    recognitionSucceeded = true;
    std::string longText;
    for (int index = 0; index < 20000; ++index)
        longText += "\xc3\xa9";
    segments = {{160, 3330, longText + "unprinted-tail"}};
    const auto bounded = library.ReinspectMedia("Transcribe the speech in that clip.", references, {true, {}});
    evidence.Check("native-transcript-bounds-and-discloses-utf8-excerpt",
        bounded.transcriptAvailable && bounded.videoTranscript.size() <= 16384 && revia::utf8::IsValid(bounded.videoTranscript) &&
            bounded.videoTranscript.find("ASR text was truncated") != std::string::npos &&
            bounded.videoTranscript.find("unprinted-tail") == std::string::npos &&
            bounded.videoTranscript.find("[00:00.160–00:03.330]") != std::string::npos);
    segments = {{160, 3330, "Courts 19. The final word is meadow."}};
    const auto mixed = library.ReinspectMedia("Transcribe the speech and describe the sampled frames.", references, {true, {}});
    evidence.Check("mixed-audio-and-visual-review-retains-normal-frame-flow",
        mixed.transcriptAvailable && mixed.images.size() == 4 && mixed.videoTimestampsMs.size() == 4 &&
            mixed.reference.find("Courts 19. The final word is meadow.") != std::string::npos);
    const auto silent = library.Stage(fixtures / "clip-01.mp4");
    Require(silent.status == IntakeStatus::Accepted, "The no-audio source could not be staged.");
    const auto callsBeforeSilent = calls;
    const auto noAudio =
        library.PrepareTurn("Transcribe the speech in the attached clip.", library.TakeStaged({silent.attachment.id}), {true, {}});
    evidence.Check("no-audio-stream-produces-no-asr-or-visual-claims",
        !noAudio.transcriptAvailable && noAudio.videoTranscript.empty() && noAudio.images.empty() && calls == callsBeforeSilent &&
            std::any_of(noAudio.notices.begin(), noAudio.notices.end(),
                [](const auto& notice) { return notice.find("no audio stream") != std::string::npos; }));
    std::string error;
    Require(library.Forget(staged.attachment.id, error) && library.Forget(silent.attachment.id, error), error);
    const auto afterForget = library.ReinspectMedia("Transcribe the speech in that clip.", references, {true, {}});
    evidence.Check("forgotten-transcript-source-cannot-resurrect-native-data",
        afterForget.videoTranscript.empty() && !afterForget.transcriptAvailable && afterForget.images.empty() && library.List().empty() &&
            std::filesystem::is_empty(root / "Files") &&
            revia::audit::ContentDigest(Read(fixtures / "clip-04.mp4")) == staged.attachment.digest);
}

void Run(Evidence& evidence, const std::filesystem::path& fixtures)
{
    const auto manifestBytes = Read(fixtures / "fixture-manifest.json", 1024U * 1024U);
    const auto manifest = Json::parse(manifestBytes);
    Require(manifest.value("kind", "") == "stage-c-neutral-local-media-fixtures",
        "This harness requires the neutral Stage C fixture manifest.");
    std::map<std::string, std::string> inputs;
    inputs["fixture-manifest.json"] = revia::audit::ContentDigest(manifestBytes);
    for (const char* collection : {"sourceImages", "clips"})
        for (const auto& item : manifest.at(collection))
        {
            const auto name = item.at("path").get<std::string>();
            Require(
                std::filesystem::path(name).filename() == std::filesystem::path(name), "A fixture manifest path is not a direct child.");
            const auto digest = revia::audit::ContentDigest(Read(fixtures / name));
            evidence.Check("fixture-digest-" + name, digest == item.at("sha256").get<std::string>(), {{"path", name}, {"sha256", digest}});
            inputs[name] = digest;
        }
    evidence.receipt["fixtureIdentity"] = inputs;
    evidence.receipt["declaredScope"] = "Actual attachment-store decoding/control checks. Session mixed-media/private-audience gates, "
                                        "model comprehension and transcription are separate acceptance.";
    VideoDecoder decoder;
    evidence.Check("installed-decoder-availability", decoder.Availability().empty(), {{"error", decoder.Availability()}});
    AttachmentLibrary library(evidence.root / "Files", evidence.root / "library.db", evidence.root / "audit.jsonl");
    const auto clip = library.Stage(fixtures / "clip-01.mp4");
    evidence.Check("stage-real-private-clip",
        clip.status == IntakeStatus::Accepted && clip.attachment.kind == AttachmentKind::Video &&
            clip.attachment.video.durationMs == 20000 && clip.attachment.poster && clip.attachment.digest == inputs.at("clip-01.mp4") &&
            clip.attachment.storedPath.parent_path() == evidence.root / "Files" && clip.attachment.storedPath != fixtures / "clip-01.mp4",
        {{"message", clip.message}, {"id", clip.attachment.id}, {"digest", clip.attachment.digest}});
    const auto privateBytes = Read(clip.attachment.storedPath);
    evidence.Check("private-copy-byte-identity", revia::audit::ContentDigest(privateBytes) == inputs.at("clip-01.mp4"));
    const auto poster = SaveFrame(evidence, "poster.png", *clip.attachment.poster);
    const auto posterRgb = DecodeRgb(evidence.root / "poster.png", 640, 360);
    const auto posterCenter = Pixel(posterRgb, 640, 320, 160);
    evidence.Check(
        "poster-does-not-establish-brief-event", !(posterCenter[0] > 180 && posterCenter[1] < 70 && posterCenter[2] < 80), poster);
    const auto files = library.TakeStaged({clip.attachment.id});
    evidence.Check(
        "take-staged-exact-source", files.size() == 1 && files.front().digest == clip.attachment.digest && library.Staged().empty());
    const ImagePolicy vision{true, {}};
    const auto first = library.PrepareTurn("Describe the supplied clip's sampled frames.", files, vision);
    evidence.receipt["defaultFrames"] = CheckFrames(evidence, "default", first, {0, 6700, 13400, 19900}, fixtures, manifest);
    const auto originalReferences = first.mediaReferences;
    evidence.Check("source-reference-current", library.ImageReferencesCurrent(originalReferences), Reference(originalReferences.front()));
    const auto playback = library.PreviewVideo(clip.attachment.id);
    evidence.Check("trusted-playback-source", playback && playback->path == clip.attachment.storedPath &&
                                                  playback->reference.id == clip.attachment.id &&
                                                  playback->reference.digest == clip.attachment.digest &&
                                                  revia::audit::ContentDigest(Read(playback->path)) == clip.attachment.digest);
    revia::memory::MemoryScope scope;
    scope.companionId = "video-store-acceptance";
    scope.participantId = "local:fixture-owner";
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    scope.audience = {revia::identity::AudienceKind::Private, "fixture-private", 1, {}};
    conversationMessage user{"user", "Inspect the supplied clip.", scope.participantId, scope};
    user.mediaReferences = originalReferences;
    const auto selected = SelectConversationMedia("Inspect 00:11.500 to 00:12.500 in that clip.", {user}, scope);
    evidence.Check(
        "followup-selects-original-clip", selected.requested && selected.references == originalReferences && selected.notice.empty());
    const auto interval = library.ReinspectMedia("Inspect 00:11.500 to 00:12.500 in that clip.", selected.references, vision);
    evidence.receipt["intervalFrames"] = CheckFrames(evidence, "interval", interval, {11500, 11900, 12200, 12500}, fixtures, manifest);
    evidence.Check("interval-keeps-clip-identity",
        interval.mediaReferences.front().id == clip.attachment.id && interval.mediaReferences.front().digest == clip.attachment.digest &&
            interval.mediaReferences.front().sampleStartMs == 11500 && interval.mediaReferences.front().sampleEndMs == 12500,
        Reference(interval.mediaReferences.front()));
    const auto emptyInterval = library.ReinspectMedia("Inspect 00:12.010 to 00:12.090 in that clip.", originalReferences, vision);
    evidence.Check("interval-without-source-frame-releases-no-content",
        emptyInterval.images.empty() && emptyInterval.videoTimestampsMs.empty() && emptyInterval.visibleMediaReferences.empty() &&
            !emptyInterval.transcriptAvailable && emptyInterval.mediaReferences.size() == 1 &&
            emptyInterval.mediaReferences.front().sampleStartMs == 12010 && emptyInterval.mediaReferences.front().sampleEndMs == 12090 &&
            !emptyInterval.notices.empty() && emptyInterval.reference.find("no video frames were supplied") != std::string::npos,
        {{"timestampsMs", emptyInterval.videoTimestampsMs}, {"notices", emptyInterval.notices}, {"reference", emptyInterval.reference}});
    const auto narrowInterval = library.ReinspectMedia("Inspect 00:12.010 to 00:12.110 in that clip.", originalReferences, vision);
    evidence.Check("narrow-interval-releases-only-inside-source-frame",
        narrowInterval.images.size() == 1 && narrowInterval.videoTimestampsMs == std::vector<std::int64_t>{12100} &&
            narrowInterval.mediaReferences.size() == 1 && narrowInterval.visibleMediaReferences == narrowInterval.mediaReferences &&
            narrowInterval.mediaReferences.front().sampleStartMs == 12010 && narrowInterval.mediaReferences.front().sampleEndMs == 12110 &&
            FrameTimestamp(narrowInterval.images.front().name) == 12100 &&
            std::all_of(narrowInterval.videoTimestampsMs.begin(), narrowInterval.videoTimestampsMs.end(),
                [](const auto timestamp) { return timestamp >= 12010 && timestamp <= 12110; }),
        {{"timestampsMs", narrowInterval.videoTimestampsMs}, {"notices", narrowInterval.notices}, {"reference", narrowInterval.reference}});
    const auto narrowFrame = SaveFrame(evidence, "narrow-interval-frame.png", narrowInterval.images.front());
    const auto narrowRgb = DecodeRgb(evidence.root / "narrow-interval-frame.png", 640, 360);
    const auto narrowCenter = Pixel(narrowRgb, 640, 320, 160);
    evidence.Check("narrow-interval-retains-actual-event-pixels",
        narrowCenter[0] > 180 && narrowCenter[1] < 70 && narrowCenter[2] < 80, narrowFrame);
    {
        revia::memory::ConversationArchive archive(revia::actions::PathToUtf8(evidence.root / "archive.db"));
        std::string error;
        evidence.Check("archive-private-source-interval",
            archive.BeginSession("video-store", error) &&
                archive.Record("video-store", "user", "Inspect 00:11.500 to 00:12.500.", error, scope, {}, interval.mediaReferences),
            {{"error", error}});
    }
    {
        revia::memory::ConversationArchive restored(revia::actions::PathToUtf8(evidence.root / "archive.db"));
        const auto tail = restored.LoadSessionTail("video-store", 5, scope);
        auto other = scope;
        other.companionId = "different-companion";
        evidence.Check("archive-restores-only-matching-scope", tail.size() == 1 &&
                                                                   tail.front().mediaReferences == interval.mediaReferences &&
                                                                   restored.LoadSessionTail("video-store", 5, other).empty());
    }
    std::stop_source stopped;
    stopped.request_stop();
    const auto cancelled = library.ReinspectMedia("Inspect 00:12.", originalReferences, vision, stopped.get_token());
    evidence.Check("cancelled-review-releases-no-content",
        cancelled.images.empty() && cancelled.visibleMediaReferences.empty() && !cancelled.transcriptAvailable);
    auto wrong = originalReferences;
    wrong.front().digest = std::string(64, '0');
    const auto invalid = library.ReinspectMedia("Inspect 00:12.", wrong, vision);
    evidence.Check("bad-reference-digest-no-poster-fallback",
        invalid.images.empty() && invalid.visibleMediaReferences.empty() && !library.ImageReferencesCurrent(wrong));
    {
        std::ofstream changed(clip.attachment.storedPath, std::ios::binary | std::ios::app);
        changed << "changed-private-evidence-copy";
    }
    const auto tampered = library.ReinspectMedia("Inspect 00:12.", originalReferences, vision);
    evidence.Check("tampered-source-releases-no-content", tampered.images.empty() && tampered.visibleMediaReferences.empty() &&
                                                              !library.ImageReferencesCurrent(originalReferences) &&
                                                              !library.PreviewVideo(clip.attachment.id));
    {
        std::ofstream restore(clip.attachment.storedPath, std::ios::binary | std::ios::trunc);
        restore.write(privateBytes.data(), static_cast<std::streamsize>(privateBytes.size()));
        restore.close();
        Require(static_cast<bool>(restore), "The private test copy could not be restored after its tamper control.");
    }
    const auto moved = evidence.root / "missing-source-control.mp4";
    std::filesystem::rename(clip.attachment.storedPath, moved);
    const auto missing = library.ReinspectMedia("Inspect 00:12.", originalReferences, vision);
    evidence.Check("missing-source-releases-no-content", missing.images.empty() && missing.visibleMediaReferences.empty() &&
                                                             !library.ImageReferencesCurrent(originalReferences) &&
                                                             !library.PreviewVideo(clip.attachment.id));
    std::filesystem::rename(moved, clip.attachment.storedPath);
    const auto image = library.Stage(fixtures / "asset-001.png");
    evidence.Check(
        "ordinary-image-intake-preserved", image.status == IntakeStatus::Accepted && image.attachment.kind == AttachmentKind::Image);
    const auto imageTurn = library.PrepareTurn("Describe the supplied image.", library.TakeStaged({image.attachment.id}), vision);
    evidence.Check("ordinary-image-frame-present", imageTurn.images.size() == 1);
    evidence.Check("ordinary-image-pixels-preserved",
        imageTurn.images.size() == 1 && imageTurn.mediaReferences.size() == 1 && imageTurn.images.front().bytes &&
            revia::audit::ContentDigest(*imageTurn.images.front().bytes) == inputs.at("asset-001.png") &&
            !IsVideoReference(imageTurn.mediaReferences.front()) &&
            library.ReinspectImages(imageTurn.mediaReferences, vision).images.size() == 1,
        SaveFrame(evidence, "ordinary-image.png", imageTurn.images.front()));
    std::string error;
    evidence.Check("forget-ordinary-image", library.Forget(image.attachment.id, error), {{"error", error}});
    error.clear();
    evidence.Check("forget-clip-deletes-private-copy",
        library.Forget(clip.attachment.id, error) && !std::filesystem::exists(clip.attachment.storedPath), {{"error", error}});
    const auto forgotten = library.ReinspectMedia("Inspect 00:12.", originalReferences, vision);
    evidence.Check("forgotten-clip-cannot-resurrect",
        forgotten.images.empty() && forgotten.visibleMediaReferences.empty() && library.List().empty() && library.Staged().empty() &&
            !library.ImageReferencesCurrent(originalReferences) && !library.PreviewVideo(clip.attachment.id) &&
            std::filesystem::is_empty(evidence.root / "Files"));
    for (const auto& [name, digest] : inputs)
        evidence.Check("fixture-preserved-" + name, revia::audit::ContentDigest(Read(fixtures / name)) == digest);
    CheckPendingTranscriptionCancellation(evidence, fixtures, false);
    CheckPendingTranscriptionCancellation(evidence, fixtures, true);
    CheckNativeTranscriptDelivery(evidence, fixtures);
    evidence.receipt["normalRootsUsed"] = false;
    evidence.receipt["mixedMediaRejection"] =
        "Session SubmitWithAttachments owns this gate; this library harness does not claim to qualify it.";
    evidence.receipt["coverageLimitation"] = "Only retained timestamped frames were supplied. Native model comprehension, OCR and physical "
                                             "playback are unverified by this harness.";
}
}

int main(const int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: ReviaVideoStoreLive <neutral-fixtures-directory> <new-evidence-directory>\n";
        return 2;
    }
    Evidence evidence;
    try
    {
        const auto fixtures = std::filesystem::canonical(revia::actions::Utf8ToPath(argv[1]));
        const auto requested = std::filesystem::absolute(revia::actions::Utf8ToPath(argv[2])).lexically_normal();
        evidence.root = std::filesystem::canonical(requested.parent_path()) / requested.filename();
        Require(!std::filesystem::exists(evidence.root), "The evidence directory already exists; preserve it and choose a new directory.");
        Require(std::filesystem::is_directory(evidence.root.parent_path()), "The evidence directory's parent must already exist.");
        const auto relative = evidence.root.lexically_relative(fixtures);
        Require(relative.empty() || *relative.begin() == "..", "Evidence must be outside the immutable fixture directory.");
        Require(std::filesystem::create_directory(evidence.root), "A fresh evidence directory could not be created.");
        evidence.created = true;
        Run(evidence, fixtures);
        evidence.Finish(true);
        std::cout << "Actual video-store checks passed; model/content review remains pending. Receipt: "
                  << revia::actions::PathToUtf8(evidence.root / "receipt.json") << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        if (evidence.created && !std::filesystem::exists(evidence.root / "receipt.json"))
        {
            try
            {
                evidence.Finish(false, error.what());
            }
            catch (const std::exception&)
            {
                std::cerr << "The final receipt could not be written; retain the existing evidence journal.\n";
            }
        }
        return 1;
    }
}
