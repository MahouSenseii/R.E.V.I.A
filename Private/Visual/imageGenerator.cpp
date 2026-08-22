#include "Visual/imageGenerator.h"

#include <chrono>
#include <ctime>
#include <httplib.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

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

bool ImageServerProcess::Start(
    const imageSettings& settings,
    const std::string& apiKey,
    std::string& outError)
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
    command << QuoteWindowsArgument(python.wstring())
        << L' ' << QuoteWindowsArgument(script.wstring())
        << L" --host " << Utf8ToWide(settings.host)
        << L" --port " << settings.port
        << L" --model " << QuoteWindowsArgument(Utf8ToWide(settings.model))
        << L" --device " << Utf8ToWide(settings.device)
        << L" --min-free-vram-mib " << settings.minimumFreeVramMiB
        << L" --steps " << settings.steps
        << L" --guidance " << settings.guidance
        << L" --width " << settings.width
        << L" --height " << settings.height
        << L" --cache-dir " << QuoteWindowsArgument(
            ResolveRuntimePath(settings.cacheDirectory).wstring());
    if (!apiKey.empty())
    {
        command << L" --api-key " << QuoteWindowsArgument(Utf8ToWide(apiKey));
    }

    std::wstring commandLine = command.str();
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::error_code directoryError;
    std::filesystem::create_directories("logs", directoryError);
    const auto openLog = [](const wchar_t* path)
    {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        return CreateFileW(
            path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    const HANDLE output = openLog(L"logs\\revia-image.stdout.log");
    const HANDLE errors = openLog(L"logs\\revia-image.stderr.log");

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
    else if (job != nullptr)
    {
        CloseHandle(job);
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
    std::lock_guard lock(mutex);
    configuration = std::move(settings);
}

bool ImageGenerator::IsEnabled() const
{
    return configuration.bEnabled;
}

bool ImageGenerator::IsAvailable(std::string& outDetail)
{
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

bool ImageGenerator::EnsureRunning(std::string& outError)
{
    if (process.IsRunning())
    {
        return true;
    }
    if (apiKey.empty())
    {
        // A loopback listener still gets a per-run key, exactly as the chat and voice
        // workers do. Local is not the same as unauthenticated.
        std::ostringstream key;
        key << std::hex << std::chrono::steady_clock::now().time_since_epoch().count()
            << std::hex << reinterpret_cast<std::uintptr_t>(this);
        apiKey = key.str();
    }
    if (!process.Start(configuration, apiKey, outError))
    {
        return false;
    }

    // The worker binds before it loads a model, so health comes back quickly even though
    // the first generation will not.
    httplib::Client client(configuration.host, configuration.port);
    client.set_connection_timeout(2);
    client.set_read_timeout(10);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(configuration.startupTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (shuttingDown.load())
        {
            outError = "Shutting down.";
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
    return false;
}

ImageResult ImageGenerator::Generate(
    const std::string& prompt,
    const std::string& negativePrompt)
{
    ImageResult result;
    std::lock_guard lock(mutex);

    std::string detail;
    if (!IsAvailable(detail))
    {
        result.message = detail;
        return result;
    }
    if (prompt.empty())
    {
        result.message = "I need something to picture.";
        return result;
    }

    std::string error;
    if (!EnsureRunning(error))
    {
        result.message = error;
        return result;
    }

    std::error_code directoryError;
    const std::filesystem::path outputDirectory =
        ResolveRuntimePath(configuration.outputPath);
    std::filesystem::create_directories(outputDirectory, directoryError);
    const std::filesystem::path target =
        outputDirectory / ("image-" + Timestamp() + ".png");

    nlohmann::json body = {
        {"prompt", prompt},
        {"outputPath", target.string()},
        {"steps", configuration.steps},
        {"guidance", configuration.guidance},
        {"width", configuration.width},
        {"height", configuration.height}
    };
    if (!negativePrompt.empty())
    {
        body["negativePrompt"] = negativePrompt;
    }

    httplib::Client client(configuration.host, configuration.port);
    client.set_connection_timeout(5);
    client.set_read_timeout(configuration.requestTimeoutSeconds);
    httplib::Headers headers;
    headers.emplace("Authorization", "Bearer " + apiKey);

    const auto started = std::chrono::steady_clock::now();
    const auto response =
        client.Post("/generate", headers, body.dump(), "application/json");
    result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    if (!response)
    {
        result.message = "The image worker stopped responding.";
        return result;
    }
    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(response->body);
        if (response->status != 200 || !parsed.value("ok", false))
        {
            result.message = "The picture could not be generated: " +
                parsed.value("error", std::string("unknown error"));
            return result;
        }
        result.succeeded = true;
        result.path = std::filesystem::path(parsed.value("path", target.string()));
        std::ostringstream summary;
        summary << parsed.value("width", configuration.width) << 'x'
            << parsed.value("height", configuration.height) << ", "
            << parsed.value("steps", configuration.steps) << " steps on "
            << parsed.value("deviceName", std::string("CPU")) << " ("
            << parsed.value("dtype", std::string("float32")) << ") using "
            << parsed.value("model", configuration.model);
        result.detail = summary.str();
        result.message = "Generated in " +
            std::to_string(static_cast<long long>(result.elapsedMilliseconds / 1000.0)) +
            "s.";
    }
    catch (const std::exception& parseError)
    {
        result.message = std::string("The image worker returned something unreadable: ") +
            parseError.what();
    }
    return result;
}

void ImageGenerator::Shutdown()
{
    shuttingDown.store(true);
    std::lock_guard lock(mutex);
    process.Stop();
}

} // namespace revia::visual
