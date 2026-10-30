#include "Audit/contentDigest.h"
#include "Visual/videoGenerator.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace
{
void Check(const bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void WaitForChild(const std::filesystem::path& output)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(output / "fixture-child.pid") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(std::filesystem::exists(output / "fixture-child.pid"), "The fixture never reached its blocking child.");
}

void CheckChildStopped(const std::filesystem::path& output)
{
    unsigned long pid = 0;
    std::ifstream(output / "fixture-child.pid") >> pid;
#ifdef _WIN32
    if (const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid))
    {
        const auto wait = WaitForSingleObject(process, 1000);
        CloseHandle(process);
        Check(wait == WAIT_OBJECT_0, "The owned worker's descendant survived cancellation.");
    }
#endif
    std::filesystem::remove(output / "fixture-child.pid");
}

void CheckPrivateScratchRemoved(const std::filesystem::path& output)
{
    std::string name;
    std::getline(std::ifstream(output / "fixture-job-directory.txt"), name);
    const auto directory = std::filesystem::path(std::u8string(name.begin(), name.end()));
    Check(directory.is_absolute() && directory.parent_path().filename() == "ReviaVideoJobs" &&
              directory.string().find(output.string()) == std::string::npos,
        "Private video scratch was placed beneath the syncable output root.");
    Check(!std::filesystem::exists(directory), "The completed or retired job retained private local scratch.");
    Check(!std::filesystem::exists(output / ".video-jobs"), "The provider created syncable private scratch beside its output.");
    std::filesystem::remove(output / "fixture-job-directory.txt");
}

#ifdef _WIN32
class EnvironmentOverride
{
  public:
    EnvironmentOverride(const wchar_t* name, const std::filesystem::path& value) : name(name)
    {
        previous.resize(32768);
        const auto size = GetEnvironmentVariableW(name, previous.data(), static_cast<DWORD>(previous.size()));
        Check(size < previous.size(), "A test environment value exceeded its bound.");
        present = size != 0;
        previous.resize(size);
        Check(SetEnvironmentVariableW(name, value.c_str()), "The disposable test environment could not be selected.");
    }
    ~EnvironmentOverride()
    {
        SetEnvironmentVariableW(name.c_str(), present ? previous.c_str() : nullptr);
    }

  private:
    std::wstring name;
    std::wstring previous;
    bool present = false;
};

class Junction
{
  public:
    Junction(std::filesystem::path link, const std::filesystem::path& target) : link(std::move(link))
    {
        Check(std::filesystem::create_directory(this->link), "The private junction test directory could not be created.");
        const auto handle = CreateFileW(this->link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        Check(handle != INVALID_HANDLE_VALUE, "The private junction test handle could not be opened.");
        const std::wstring substitute = L"\\??\\" + target.native();
        const auto& printable = target.native();
        const auto substituteBytes = substitute.size() * sizeof(wchar_t);
        const auto printableBytes = printable.size() * sizeof(wchar_t);
        std::vector<unsigned char> buffer(16 + substituteBytes + 2 + printableBytes + 2);
        auto* words = reinterpret_cast<WORD*>(buffer.data());
        *reinterpret_cast<DWORD*>(buffer.data()) = IO_REPARSE_TAG_MOUNT_POINT;
        words[2] = static_cast<WORD>(buffer.size() - 8);
        words[4] = 0;
        words[5] = static_cast<WORD>(substituteBytes);
        words[6] = static_cast<WORD>(substituteBytes + 2);
        words[7] = static_cast<WORD>(printableBytes);
        std::copy_n(reinterpret_cast<const unsigned char*>(substitute.data()), substituteBytes, buffer.data() + 16);
        std::copy_n(reinterpret_cast<const unsigned char*>(printable.data()), printableBytes, buffer.data() + 18 + substituteBytes);
        DWORD returned = 0;
        const auto made = DeviceIoControl(
            handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
        CloseHandle(handle);
        Check(made, "The actual NTFS junction negative control could not be created.");
    }
    ~Junction()
    {
        std::error_code ignored;
        std::filesystem::remove(link, ignored);
    }

  private:
    std::filesystem::path link;
};
#endif
}

int main(const int argc, char** argv)
{
    if (argc != 5)
    {
        std::cerr << "Usage: videoGeneratorTests <python> <fixture> <ffmpeg> <ffprobe>\n";
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("revia-video-generator-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        using namespace revia::visual;
        const auto model = root / "model";
        std::filesystem::create_directories(model / "_runtime" / "site-packages");
        std::ofstream(model / "model_index.json") << "{}";
        videoSettings settings;
        settings.bEnabled = true;
        settings.pythonExecutable = argv[1];
        settings.workerScript = argv[2];
        settings.modelDirectory = model.string();
        settings.runtimeOverlayDirectory = (model / "_runtime" / "site-packages").string();
        settings.outputPath = (root / "outputs").string();
        settings.ffmpegPath = argv[3];
        settings.ffprobePath = argv[4];
        settings.requestTimeoutSeconds = 10;
        VideoGenerator generator;
        generator.Configure(settings);
        std::string error;
        Check(generator.IsAvailable(error), "The CPU fixture configuration was unavailable: " + error);
        VideoRequest request;
        request.prompt = "valid";
        request.seed = 42;
        const auto valid = generator.Generate(request);
        Check(valid.succeeded, "A real 49-frame CPU fixture MP4 was refused: " + valid.message + " " + valid.detail);
        Check(valid.receipt.seed == 42 && valid.receipt.frameCount == 49 && valid.receipt.durationMs == 1960,
            "The native result lost its bound generation metadata.");
        Check(valid.receipt.workerPid > 0 && valid.receipt.peakWorkerRssMiB > 0 && !valid.receipt.processTreeMemoryMeasured &&
                  valid.receipt.minimumFreeSystemRamBytes >= 8ULL * 1024 * 1024 * 1024,
            "The actual fixture process resource measurements were lost or attributed to the process tree.");
        CheckPrivateScratchRemoved(root / "outputs");
#ifdef _WIN32
        const auto synced = root / "synced-temp";
        std::filesystem::create_directory(synced);
        {
            EnvironmentOverride temporary(L"TEMP", synced);
            EnvironmentOverride alternate(L"TMP", synced);
            EnvironmentOverride oneDrive(L"OneDrive", root);
            const auto refused = generator.Generate(request);
            Check(!refused.succeeded && refused.message.find("private owned") != std::string::npos &&
                      !std::filesystem::exists(root / "outputs" / "fixture-job-directory.txt"),
                "A known synced TEMP root was used for private source pixels or launched a worker.");
        }
        {
            const auto link = root / "linked-output";
            Junction junction(link, root / "outputs");
            settings.outputPath = link.string();
            generator.Configure(settings);
            const auto refused = generator.Generate(request);
            Check(!refused.succeeded && refused.message.find("output directory") != std::string::npos &&
                      !std::filesystem::exists(root / "outputs" / "fixture-job-directory.txt"),
                "A junction output root was resolved past its lexical ownership guard.");
        }
        settings.outputPath = (root / "outputs").string();
        generator.Configure(settings);
#endif
        request.prompt = "delayed exit";
        const auto delayed = generator.Generate(request);
        Check(delayed.succeeded, "Closed stdout was mistaken for a failed process before actual exit: " + delayed.message);
        CheckPrivateScratchRemoved(root / "outputs");
        request.prompt = "valid";
        const revia::attachments::VideoDecoderSettings decoder{argv[3], argv[4]};
        auto wrong = valid.receipt;
        wrong.seed ^= 1;
        Check(!VerifyVideoArtifact(valid.expectedArtifact, wrong, decoder, {}, {}, error), "A different seed was accepted.");
        wrong = valid.receipt;
        wrong.mode = VideoRequestMode::ImageToVideo;
        Check(!VerifyVideoArtifact(valid.expectedArtifact, wrong, decoder, {}, {}, error), "A foreign mode was accepted.");
        const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1, 8, 4, 0, 0, 0,
            181, 28, 12, 2, 0, 0, 0, 11, 73, 68, 65, 84, 120, 218, 99, 100, 248, 15, 0, 1, 5, 1, 1, 39, 24, 227, 102, 0, 0, 0, 0, 73, 69,
            78, 68, 174, 66, 96, 130};
        const auto bytes = std::make_shared<const std::string>(reinterpret_cast<const char*>(png), sizeof(png));
        request.mode = VideoRequestMode::ImageToVideo;
        request.source = ImageSource{"source-id", revia::audit::ContentDigest(*bytes), "image/png", bytes, 1, 1};
        const auto image = generator.Generate(request);
        Check(image.succeeded && image.receipt.sourceDigest == request.source->digest,
            "The typed reference did not survive transport: " + image.message);
        CheckPrivateScratchRemoved(root / "outputs");
        for (const auto* prompt : {"wrong seed", "wrong source", "wrong device", "wrong frame count", "wrong timestamps", "corrupt"})
        {
            request.prompt = prompt;
            const auto rejected = generator.Generate(request);
            Check(!rejected.succeeded && rejected.path.empty(), std::string("Malformed artifact was published: ") + prompt);
            CheckPrivateScratchRemoved(root / "outputs");
        }
        request.prompt = "wait";
        std::stop_source stop;
        auto active = std::async(std::launch::async, [&]() { return generator.Generate(request, stop.get_token()); });
        WaitForChild(root / "outputs");
        const auto statusAt = std::chrono::steady_clock::now();
        Check(generator.IsEnabled() && generator.IsAvailable(error) && generator.Snapshot().jobId.size() > 0,
            "Status became unavailable while the video worker was active.");
        Check(std::chrono::steady_clock::now() - statusAt < std::chrono::milliseconds(100), "Video status waited behind inference.");
        const auto busy = generator.Generate(request);
        Check(!busy.succeeded && busy.message.find("busy") != std::string::npos, "A second video job was admitted.");
        const auto cancelledAt = std::chrono::steady_clock::now();
        stop.request_stop();
        Check(active.get().cancelled, "Caller cancellation was ignored.");
        Check(std::chrono::steady_clock::now() - cancelledAt < std::chrono::seconds(2), "Cancellation waited for the model deadline.");
        CheckChildStopped(root / "outputs");
        CheckPrivateScratchRemoved(root / "outputs");
        active = std::async(std::launch::async, [&]() { return generator.Generate(request); });
        WaitForChild(root / "outputs");
        const auto directCancelAt = std::chrono::steady_clock::now();
        generator.Cancel();
        Check(std::chrono::steady_clock::now() - directCancelAt < std::chrono::milliseconds(100), "Cancel waited behind inference.");
        Check(active.get().cancelled, "Provider cancellation was ignored.");
        CheckChildStopped(root / "outputs");
        CheckPrivateScratchRemoved(root / "outputs");
        request.prompt = "flood";
        const auto flood = generator.Generate(request);
        Check(!flood.succeeded && flood.message.find("metadata bound") != std::string::npos,
            "A growing result file was not bounded before exit.");
        CheckChildStopped(root / "outputs");
        CheckPrivateScratchRemoved(root / "outputs");
        settings.requestTimeoutSeconds = 1;
        generator.Configure(settings);
        request.prompt = "timeout";
        const auto timeout = generator.Generate(request);
        Check(!timeout.succeeded && !timeout.cancelled && timeout.message.find("time limit") != std::string::npos,
            "The owned job deadline was not enforced distinctly from caller cancellation.");
        CheckChildStopped(root / "outputs");
        CheckPrivateScratchRemoved(root / "outputs");
        request.mode = VideoRequestMode::TextToVideo;
        Check(!generator.Generate(request).succeeded, "Text-to-video silently discarded source pixels.");
        request.mode = VideoRequestMode::ImageToVideo;
        request.source.reset();
        Check(!generator.Generate(request).succeeded, "Image-to-video silently ran without a source.");
        generator.Shutdown();
        std::filesystem::remove_all(root);
        std::cout << "CPU video artifact, provenance, complete decode, source, deadline and owned-tree checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
