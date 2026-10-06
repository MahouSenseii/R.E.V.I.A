#include "Visual/imageSettings.h"
#include "Visual/imageGenerator.h"
#include "Visual/imageArtifact.h"
#include "Core/localApiKey.h"
#include "Core/logger.h"

#include <chrono>
#include <ctime>
#include <httplib.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>
#include <thread>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::visual
{

namespace
{

std::string Timestamp()
{
    const std::time_t time = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H-%M-%SZ");
    return stream.str();
}

std::filesystem::path ResolveRuntimePath(const std::string& configured)
{
    const std::filesystem::path value(configured);
    if (value.is_absolute())
    {
        return value.lexically_normal();
    }
    std::error_code error;
    const std::filesystem::path current =
        std::filesystem::absolute(value, error).lexically_normal();
    if (!error && std::filesystem::exists(current))
    {
        return current;
    }
#ifdef _WIN32
    std::vector<wchar_t> module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length > 0 && length < module.size())
    {
        const std::filesystem::path executableDirectory =
            std::filesystem::path(std::wstring(module.data(), length)).parent_path();
        for (const std::filesystem::path& root : {
            executableDirectory,
            executableDirectory.parent_path(),
            executableDirectory.parent_path().parent_path()})
        {
            const std::filesystem::path candidate = (root / value).lexically_normal();
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }
    }
#endif
    return current;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), length);
    return output;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument)
{
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        return argument;
    }
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}
#endif

} // namespace

ImageServerProcess::~ImageServerProcess()
{
    if (bShutdownOnExit)
    {
        Stop();
    }
}

bool ImageServerProcess::Start(const imageSettings& settings, const std::string& apiKey, std::string& outError)
{
    bShutdownOnExit = settings.bShutdownOnExit;
#ifndef _WIN32
    (void)settings;
    (void)apiKey;
    outError = "The local image worker is currently implemented for Windows.";
    return false;
#else
    if (IsRunning())
    {
        return true;
    }
    Stop();

    const std::filesystem::path python = ResolveRuntimePath(settings.pythonExecutable);
    const std::filesystem::path script = ResolveRuntimePath(settings.serviceScript);
    if (!std::filesystem::is_regular_file(python))
    {
        outError = "The image runtime is not installed. Run Tools/InstallImageModel.ps1 "
            "to create it, then try again.";
        return false;
    }
    if (!std::filesystem::is_regular_file(script))
    {
        outError = "The image worker script is missing: " + script.string();
        return false;
    }

    std::wostringstream command;
    command << QuoteWindowsArgument(python.wstring()) << L' ' << QuoteWindowsArgument(script.wstring()) << L" --host "
            << Utf8ToWide(settings.host) << L" --port " << settings.port << L" --model " << QuoteWindowsArgument(Utf8ToWide(settings.model))
            << L" --device " << Utf8ToWide(settings.device) << L" --min-free-vram-mib " << settings.minimumFreeVramMiB << L" --steps "
            << settings.steps << L" --guidance " << settings.guidance << L" --width " << settings.width << L" --height " << settings.height
            << L" --gpu-reserve-mib " << settings.gpuReserveMiB << L" --cpu-threads " << settings.cpuThreads << L" --output-root "
            << QuoteWindowsArgument(ResolveRuntimePath(settings.outputPath).wstring()) << L" --cache-dir "
            << QuoteWindowsArgument(ResolveRuntimePath(settings.cacheDirectory).wstring());
    if (settings.bKeepLoaded)
        command << L" --keep-loaded";
    if (settings.bOffline)
        command << L" --offline";
    if (!settings.variant.empty())
        command << L" --variant " << QuoteWindowsArgument(Utf8ToWide(settings.variant));
    if (!apiKey.empty())
    {
        command << L" --api-key " << QuoteWindowsArgument(Utf8ToWide(apiKey));
    }

    std::wstring commandLine = command.str();
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::error_code directoryError;
    std::filesystem::create_directories((settings.logDirectory.empty() ? ReviaLogDirectory() : settings.logDirectory), directoryError);
    const auto openLog = [](const wchar_t* path)
    {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        return CreateFileW(
            path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    const HANDLE output = openLog((std::filesystem::path((settings.logDirectory.empty() ? ReviaLogDirectory() : settings.logDirectory)) / L"revia-image.stdout.log").wstring().c_str());
    const HANDLE errors = openLog((std::filesystem::path((settings.logDirectory.empty() ? ReviaLogDirectory() : settings.logDirectory)) / L"revia-image.stderr.log").wstring().c_str());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output;
    startup.hStdError = errors;
    PROCESS_INFORMATION information{};
    const std::wstring workingDirectory =
        std::filesystem::current_path(directoryError).wstring();
    const BOOL created = CreateProcessW(
        nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startup, &information);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (errors != INVALID_HANDLE_VALUE) CloseHandle(errors);
    if (!created)
    {
        outError = "The image worker process could not be started.";
        return false;
    }

    // Kill-on-close, so a worker holding a multi-gigabyte model cannot outlive Revia.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job != nullptr && SetInformationJobObject(
            job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
        AssignProcessToJobObject(job, information.hProcess))
    {
        jobHandle = job;
    }
    else
    {
        if (job != nullptr)
            CloseHandle(job);
        TerminateProcess(information.hProcess, 1);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
        outError = "The image worker could not obtain owned process lifetime.";
        return false;
    }
    if (ResumeThread(information.hThread) == static_cast<DWORD>(-1))
    {
        if (jobHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(jobHandle));
            jobHandle = nullptr;
        }
        TerminateProcess(information.hProcess, 1);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
        outError = "The image worker could not be resumed.";
        return false;
    }
    CloseHandle(information.hThread);
    processHandle = information.hProcess;
    return true;
#endif
}

bool ImageServerProcess::IsRunning() const
{
#ifdef _WIN32
    if (processHandle == nullptr)
    {
        return false;
    }
    DWORD exitCode = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode) &&
        exitCode == STILL_ACTIVE;
#else
    return false;
#endif
}

bool ImageServerProcess::WasStartedByRevia() const
{
#ifdef _WIN32
    return processHandle != nullptr;
#else
    return false;
#endif
}

void ImageServerProcess::Stop()
{
#ifdef _WIN32
    if (processHandle == nullptr)
    {
        return;
    }
    const HANDLE handle = static_cast<HANDLE>(processHandle);
    TerminateProcess(handle, 0);
    WaitForSingleObject(handle, 5000);
    CloseHandle(handle);
    if (jobHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(jobHandle));
        jobHandle = nullptr;
    }
    processHandle = nullptr;
#endif
}

ImageGenerator::~ImageGenerator()
{
    Shutdown();
}

void ImageGenerator::Configure(imageSettings settings)
{
    Cancel();
    std::lock_guard lock(mutex);
    process.Stop();
    configuration = std::move(settings);
    enabled.store(configuration.bEnabled);
    shuttingDown.store(false);
    ImageJobSnapshot initial;
    initial.state = configuration.bEnabled ? "idle" : "disabled";
    initial.model = configuration.model;
    SetSnapshot(std::move(initial));
}

bool ImageGenerator::IsEnabled() const
{
    return enabled.load();
}

std::filesystem::path ImageGenerator::OutputDirectory()
{
    std::lock_guard lock(mutex);
    return ResolveRuntimePath(configuration.outputPath);
}

bool ImageGenerator::IsAvailable(std::string& outDetail)
{
    std::lock_guard lock(mutex);
    if (!configuration.bEnabled)
    {
        outDetail = "Image generation is off. Set image.enabled true in "
                    "Config/settings.json once the runtime is installed.";
        return false;
    }
    const std::filesystem::path python =
        ResolveRuntimePath(configuration.pythonExecutable);
    if (!std::filesystem::is_regular_file(python))
    {
        outDetail = "The image runtime is not installed. Run Tools/InstallImageModel.ps1.";
        return false;
    }
    outDetail = "Image generation is available through " + configuration.model + '.';
    return true;
}

bool ImageGenerator::EnsureRunning(std::string& outError, const std::function<bool()>& cancelled)
{
    if (process.IsRunning())
    {
        return true;
    }
    if (apiKey.empty())
    {
        // A loopback listener still gets a per-run key, exactly as the chat and voice
        // workers do. Local is not the same as unauthenticated. From the same generator
        // they use, too: the clock reading and object address this used to be made of are
        // both guessable, and the key guards an endpoint that writes a file where told.
        apiKey = core::GenerateLocalApiKey();
    }
    if (!process.Start(configuration, apiKey, outError))
    {
        return false;
    }

    // The worker binds before it loads a model, so health comes back quickly even though
    // the first generation will not.
    httplib::Client client(configuration.host, configuration.port);
    client.set_connection_timeout(1);
    client.set_read_timeout(1);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(configuration.startupTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cancelled())
        {
            process.Stop();
            outError = "Image generation was cancelled.";
            return false;
        }
        httplib::Headers headers;
        headers.emplace("Authorization", "Bearer " + apiKey);
        if (const auto response = client.Get("/health", headers);
            response && response->status == 200)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    outError = "The image worker did not become ready within " +
        std::to_string(configuration.startupTimeoutSeconds) + " seconds.";
    process.Stop();
    return false;
}

ImageResult ImageGenerator::Generate(const std::string& prompt, const std::string& negativePrompt, const std::stop_token stopToken,
    std::function<bool()> admission, const std::filesystem::path& expectedOutputRoot)
{
    ImageResult result;
    const auto generation = cancellationGeneration.load();
    std::lock_guard lock(mutex);
    const auto cancelled = [&]()
    {
        return shuttingDown.load() || stopToken.stop_requested() || generation != cancellationGeneration.load() ||
               (admission && !admission());
    };
    if (!expectedOutputRoot.empty())
    {
        std::error_code error;
        const auto currentRoot = std::filesystem::weakly_canonical(ResolveRuntimePath(configuration.outputPath), error);
        if (error || currentRoot != expectedOutputRoot)
        {
            result.message = "The image provider destination changed after admission.";
            return result;
        }
    }
    if (!configuration.bEnabled)
    {
        result.message = "Image generation is disabled. Enable the installed image provider in settings.";
        return result;
    }
    if (prompt.empty())
    {
        result.message = "I need something to picture.";
        return result;
    }
    if (cancelled())
    {
        result.cancelled = true;
        result.message = "Image generation was cancelled before it started.";
        return result;
    }
    ImageJobSnapshot progress;
    progress.state = "starting";
    progress.model = configuration.model;
    progress.jobId = core::GenerateLocalApiKey();
    SetSnapshot(progress);
    std::string error;
    if (!EnsureRunning(error, cancelled))
    {
        result.message = error;
        result.cancelled = cancelled();
        progress.state = result.cancelled ? "cancelled" : "failed";
        progress.detail = error;
        SetSnapshot(progress);
        return result;
    }

    std::error_code directoryError;
    const std::filesystem::path outputDirectory =
        ResolveRuntimePath(configuration.outputPath);
    std::filesystem::create_directories(outputDirectory, directoryError);
    const std::filesystem::path target = outputDirectory / ("image-" + Timestamp() + "-" + progress.jobId + ".png");
    if (directoryError || std::filesystem::exists(target))
    {
        result.message = "The image output directory is unavailable or the artifact already exists.";
        progress.state = "failed";
        progress.detail = result.message;
        SetSnapshot(progress);
        return result;
    }

    nlohmann::json body = {{"jobId", progress.jobId}, {"prompt", prompt},
        {"outputPath", std::string(reinterpret_cast<const char*>(target.u8string().c_str()))}, {"steps", configuration.steps},
        {"guidance", configuration.guidance}, {"width", configuration.width}, {"height", configuration.height}};
    if (configuration.seed >= 0)
        body["seed"] = configuration.seed;
    if (!negativePrompt.empty())
    {
        body["negativePrompt"] = negativePrompt;
    }

    httplib::Client client(configuration.host, configuration.port);
    client.set_connection_timeout(1);
    client.set_read_timeout(2);
    client.set_write_timeout(2);
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + apiKey);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(configuration.requestTimeoutSeconds);
    try
    {
        const auto response = client.Post("/jobs", headers, body.dump(), "application/json");
        if (!response || response->status != 202)
        {
            throw std::runtime_error("The image worker could not admit the job." +
                                     (response ? " " + nlohmann::json::parse(response->body).value("error", std::string()) : ""));
        }
        while (true)
        {
            if (cancelled())
            {
                result.cancelled = true;
                throw std::runtime_error("Image generation was cancelled.");
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error("Image generation exceeded its time limit.");
            }
            const auto poll = client.Get("/jobs/" + progress.jobId, headers);
            if (!poll || poll->status != 200)
            {
                throw std::runtime_error("The image worker stopped responding.");
            }
            const nlohmann::json parsed = nlohmann::json::parse(poll->body);
            if (parsed.at("jobId").get<std::string>() != progress.jobId)
            {
                throw std::runtime_error("The image worker returned another job's status.");
            }
            progress.state = parsed.at("state").get<std::string>();
            progress.step = parsed.value("step", 0);
            progress.steps = parsed.value("steps", 0);
            progress.device = parsed.value("device", std::string());
            progress.loaded = parsed.value("loaded", false);
            progress.detail =
                progress.state +
                (progress.steps > 0 ? ": " + std::to_string(progress.step) + "/" + std::to_string(progress.steps) + " steps" : "");
            SetSnapshot(progress);
            if (progress.state == "cancelled" || progress.state == "failed")
            {
                result.cancelled = progress.state == "cancelled";
                throw std::runtime_error(parsed.value("error", std::string("The image job failed.")));
            }
            if (progress.state == "succeeded")
            {
                ImageArtifactReceipt receipt;
                const auto returnedPath = parsed.at("path").get<std::string>();
                receipt.path = std::filesystem::path(std::u8string(returnedPath.begin(), returnedPath.end()));
                receipt.jobId = parsed.at("jobId").get<std::string>();
                receipt.model = parsed.at("model").get<std::string>();
                receipt.sha256 = parsed.at("sha256").get<std::string>();
                receipt.width = parsed.at("width").get<int>();
                receipt.height = parsed.at("height").get<int>();
                const int width = std::clamp(configuration.width, 256, 1024) / 8 * 8;
                const int height = std::clamp(configuration.height, 256, 1024) / 8 * 8;
                if (!VerifyImageArtifact(target, progress.jobId, configuration.model, width, height, receipt, error))
                {
                    throw std::runtime_error(error);
                }
                if (cancelled())
                {
                    result.cancelled = true;
                    throw std::runtime_error("Image generation was cancelled before publication.");
                }
                result.succeeded = true;
                result.receipt = receipt;
                result.modelLoadMilliseconds = parsed.value("loadElapsedMs", 0.0);
                result.inferenceMilliseconds = parsed.value("elapsedMs", 0.0);
                result.peakAllocatedMiB = parsed.value("peakAllocatedMiB", 0.0);
                result.peakReservedMiB = parsed.value("peakReservedMiB", 0.0);
                result.path = target;
                result.detail = std::to_string(width) + "x" + std::to_string(height) + ", " + std::to_string(progress.steps) +
                                " steps on " + parsed.value("deviceName", std::string("unknown")) + " using " + receipt.model +
                                "; decoded PNG and SHA256 verified; visual quality requires review.";
                result.message = "Generated and verified the image file.";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    catch (const std::exception& failure)
    {
        result.succeeded = false;
        result.path.clear();
        result.cancelled = result.cancelled || cancelled();
        result.message = failure.what();
        process.Stop();
        std::filesystem::remove(target, directoryError);
        std::filesystem::remove(target.parent_path() / (target.stem().string() + ".pending.png"), directoryError);
        progress.state = result.cancelled ? "cancelled" : "failed";
        progress.loaded = false;
        progress.detail = result.message;
    }
    result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (result.succeeded && !configuration.bKeepLoaded)
    {
        process.Stop();
        progress.loaded = false;
    }
    SetSnapshot(progress);
    return result;
}

void ImageGenerator::Cancel()
{
    cancellationGeneration.fetch_add(1);
}

ImageJobSnapshot ImageGenerator::Snapshot() const
{
    std::lock_guard lock(statusMutex);
    return status;
}

void ImageGenerator::SetSnapshot(ImageJobSnapshot value)
{
    std::lock_guard lock(statusMutex);
    status = std::move(value);
}

void ImageGenerator::Unload()
{
    Cancel();
    std::lock_guard lock(mutex);
    process.Stop();
    auto snapshot = Snapshot();
    snapshot.loaded = false;
    snapshot.state = "unloaded";
    SetSnapshot(std::move(snapshot));
}

void ImageGenerator::Shutdown()
{
    shuttingDown.store(true);
    Unload();
}

} // namespace revia::visual
