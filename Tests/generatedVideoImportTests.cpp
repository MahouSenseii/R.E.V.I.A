#include "Attachments/attachmentLibrary.h"
#include "Audit/contentDigest.h"
#include "Core/runtimePath.h"
#include "Process/ownedProcess.h"
#include "Visual/videoTypes.h"
#include "testSupport.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace
{
using revia::tests::Check;

std::string Read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

revia::attachments::VideoDecoderSettings DecoderSettings()
{
    const auto installed = [](const std::filesystem::path& relative)
    {
        const auto value = revia::core::FindInstalledArtifact(revia::core::RuntimeRoot(), relative);
        return value.empty() ? revia::core::ResolveRuntimePath(relative) : value;
    };
    return {installed("ThirdParty/ffmpeg/bin/ffmpeg.exe"), installed("ThirdParty/ffmpeg/bin/ffprobe.exe")};
}

revia::visual::VideoResult VideoFixture(
    const std::filesystem::path& outputRoot, const std::string& job, const revia::attachments::VideoDecoderSettings& decoder)
{
    std::filesystem::create_directories(outputRoot);
    revia::visual::VideoResult result;
    result.succeeded = true;
    result.path = outputRoot / ("video-" + job + ".mp4");
    revia::process::OwnedProcess child;
    revia::process::OwnedProcessRequest command;
    command.executable = decoder.ffmpegPath;
    command.arguments = {"-nostdin", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=navy:s=384x256:r=25:d=1.96",
        "-frames:v", "49", "-an", "-c:v", "libopenh264", "-threads", "1", "-pix_fmt", "yuv420p", "-n",
        revia::actions::PathToUtf8(result.path)};
    command.maxPendingOutputBytes = 65536;
    std::string error;
    Check(child.Start(command, error), "The manual real-FFmpeg fixture could not start: " + error);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((child.Running() || !child.ExitCode()) && std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(child.Read(std::chrono::milliseconds(10)));
        if (!child.Running() && !child.ExitCode())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!child.Running() && child.ExitCode() == 0, "The manual real-FFmpeg fixture failed: " + child.ErrorOutput());
    auto& expected = result.expectedArtifact;
    expected.path = result.path;
    expected.jobId = job;
    expected.seed = 42;
    auto& receipt = result.receipt;
    receipt.path = result.path;
    receipt.sha256 = revia::audit::ContentDigest(Read(result.path));
    receipt.jobId = job;
    receipt.model = expected.model;
    receipt.revision = expected.revision;
    receipt.mode = expected.mode;
    receipt.seed = expected.seed;
    receipt.width = expected.width;
    receipt.height = expected.height;
    receipt.frameCount = expected.frameCount;
    receipt.fps = expected.fps;
    receipt.durationMs = expected.durationMs;
    receipt.steps = receipt.denoisingSteps = expected.steps;
    receipt.provider = "local_diffusers_ltx";
    receipt.pipeline = "LTXPipeline";
    receipt.device = "cuda:1";
    receipt.dtype = "float32";
    receipt.offload = "sequential_cpu";
    receipt.cpuThreads = 6;
    receipt.workerPid = 1;
    receipt.peakWorkerRssMiB = 1;
    receipt.minimumFreeSystemRamBytes = 8ULL * 1024 * 1024 * 1024;
    return result;
}
}

void RunGeneratedVideoImportTests()
{
    using namespace revia::attachments;
    const auto decoder = DecoderSettings();
    Check(VideoDecoder(decoder).Availability().empty(), "This manual suite requires actual local FFmpeg/ffprobe; no video checks ran.");
    revia::tests::ScopedTestDirectory directory;
    const auto files = directory.root / "files", database = directory.root / "library.db", audit = directory.root / "audit.jsonl";
    const auto output = directory.root / "output";
    auto fixture = VideoFixture(output, "video-transfer", decoder);
    std::string error;
    const bool verified =
        revia::visual::VerifyVideoArtifact(fixture.expectedArtifact, fixture.receipt, decoder, {}, [] { return true; }, error);
    Check(verified, "The structural CPU video fixture failed strict native verification: " + error);
    AttachmentLibrary library(files, database, audit, {}, decoder);
    const auto revision = library.MediaRevision();
    const auto imported = library.ImportGeneratedVideo(fixture, output, [] { return true; });
    Check(imported.succeeded && imported.reference.origin == MediaOrigin::GeneratedArtifact && imported.reference.mimeType == "video/mp4" &&
              imported.reference.width == 384 && imported.reference.height == 256 && imported.reference.durationMs == 1960 &&
              imported.reference.sampleStartMs == 0 && imported.reference.sampleEndMs == 1959 && imported.poster &&
              imported.poster->bytes && !std::filesystem::exists(fixture.path) && std::filesystem::exists(imported.storedPath),
        "Generated video transfer lost canonical ownership, exact metadata or real poster: " + imported.error);
    Check(library.MediaRevision() == revision && library.Staged().empty(), "Video import retired its own source or became an upload.");
    Check(library.ImageReferencesCurrent({imported.reference}) && !library.ReadImageSource(imported.reference),
        "Generated video source identity was lost or promoted into a static img2img source.");
    const auto playback = library.PreviewVideo(imported.reference.id);
    Check(playback && playback->reference == imported.reference && playback->info.width == 384 && playback->info.durationMs == 1960,
        "The generated canonical video did not reopen through trusted playback.");
    const auto inspected = library.ReinspectMedia("Inspect that clip around 00:00.500.", {imported.reference}, {true, {}});
    Check(!inspected.images.empty() && inspected.visibleMediaReferences.front().origin == MediaOrigin::GeneratedArtifact,
        "Generated video follow-up did not yield current actual timestamped pixels.");
    auto forged = imported.reference;
    forged.origin = MediaOrigin::UserAttachment;
    Check(!library.ImageReferencesCurrent({forged}), "A generated video was authenticated as an upload.");
    const auto documents = library.List();
    Check(documents.size() == 1 && documents.front().generated && documents.front().generated->mode == "text_to_video" &&
              documents.front().generated->modelRevision == fixture.expectedArtifact.revision,
        "Generated video model/mode/revision provenance was lost.");
    Check(library.Forget(imported.reference.id, error) && !std::filesystem::exists(imported.storedPath) &&
              !library.ImageReferencesCurrent({imported.reference}),
        "Video Forget did not remove the sole canonical output: " + error);
    auto expired = VideoFixture(output, "video-expired", decoder);
    const auto cleanupStarted = std::chrono::steady_clock::now();
    Check(!library.ImportGeneratedVideo(expired, output, [] { return false; }).succeeded && !std::filesystem::exists(expired.path) &&
              library.List().empty(),
        "Retired video import left an unpublished output or library association.");
    const auto cleanupMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cleanupStarted).count();
    std::cout << "Expired-turn exact video cleanup: " << cleanupMs << " ms (native CPU verification only).\n";
    Check(cleanupMs < 35000, "Expired video cleanup exceeded the fixed native verification time bounds.");
    auto cancelled = VideoFixture(output, "video-cancelled", decoder);
    std::stop_source stop;
    stop.request_stop();
    Check(!library.ImportGeneratedVideo(
                      cancelled, output, [] { return true; }, stop.get_token())
                  .succeeded &&
              !std::filesystem::exists(cancelled.path),
        "Cancelled video import left a verified unpublished output.");
    auto late = VideoFixture(output, "video-late-commit", decoder);
    const auto lateResult = library.ImportGeneratedVideo(late, output, [&] { return library.List().empty(); });
    Check(!lateResult.succeeded && !std::filesystem::exists(late.path) && library.List().empty(),
        "Retirement after video library commit left an association or either output copy.");
    auto blockedCleanup = VideoFixture(output, "video-cleanup-blocked", decoder);
    std::filesystem::permissions(blockedCleanup.path, std::filesystem::perms::owner_read);
    const auto blockedResult = library.ImportGeneratedVideo(blockedCleanup, output, [] { return false; });
    std::filesystem::permissions(blockedCleanup.path, std::filesystem::perms::owner_all);
    Check(!blockedResult.succeeded && std::filesystem::exists(blockedCleanup.path) && library.List().empty() &&
              blockedResult.error.find("could not be removed") != std::string::npos,
        "A blocked exact-output cleanup did not report its operational failure.");
    auto foreign = VideoFixture(directory.root / "foreign", "video-foreign", decoder);
    Check(!library.ImportGeneratedVideo(foreign, output, [] { return true; }).succeeded && std::filesystem::exists(foreign.path),
        "Video import accepted or deleted a foreign-root file.");
    std::cout << "Manual generated-video import checked actual 49-frame CPU MP4 transfer, poster, follow-up, provenance, Forget and "
                 "retirement.\n";
    std::cout << "Synthetic structural fixtures do not certify model generation, motion quality or source fidelity.\n";
}

#ifdef REVIA_GENERATED_VIDEO_IMPORT_STANDALONE
int main()
{
    try
    {
        RunGeneratedVideoImportTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Generated video import failure: " << error.what() << '\n';
        return 1;
    }
}
#endif
