#include "LLM/LLamaCPP/llamaCppServerProcess.h"

#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <dxgi1_2.h>
#include <windows.h>
#endif

namespace
{

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    const int requiredLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (requiredLength <= 0)
    {
        return {};
    }

    std::wstring converted(static_cast<std::size_t>(requiredLength), L'\0');
    const int convertedLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        converted.data(),
        requiredLength);
    if (convertedLength != requiredLength)
    {
        return {};
    }
    return converted;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument)
{
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        return argument;
    }

    std::wstring quoted = L"\"";
    std::size_t backslashCount = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashCount;
            continue;
        }

        if (character == L'\"')
        {
            quoted.append(backslashCount * 2 + 1, L'\\');
            quoted.push_back(character);
            backslashCount = 0;
            continue;
        }

        quoted.append(backslashCount, L'\\');
        backslashCount = 0;
        quoted.push_back(character);
    }

    quoted.append(backslashCount * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::string WindowsError(const std::string& operation)
{
    return operation + " failed with Windows error " + std::to_string(GetLastError()) + ".";
}

std::filesystem::path ResolveRuntimePath(const std::string& configured)
{
    const std::filesystem::path value(configured);
    if (value.is_absolute())
    {
        return value.lexically_normal();
    }
    std::error_code error;
    const std::filesystem::path current = std::filesystem::absolute(value, error).lexically_normal();
    if (!error && std::filesystem::exists(current))
    {
        return current;
    }
    std::vector<wchar_t> module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
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
    return current;
}

std::uint64_t DedicatedVideoMemoryMiB()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory))))
    {
        return 0;
    }

    std::uint64_t largestBytes = 0;
    for (UINT index = 0;; ++index)
    {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(result) || !adapter)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            largestBytes = std::max<std::uint64_t>(
                largestBytes,
                description.DedicatedVideoMemory);
        }
        adapter->Release();
    }
    factory->Release();
    return largestBytes / (1024ull * 1024ull);
}

std::uint64_t SystemMemoryMiB()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status)
        ? status.ullTotalPhys / (1024ull * 1024ull)
        : 0;
}

int AutomaticContextSize(
    const std::uint64_t videoMemory,
    const std::uint64_t systemMemory)
{
    int gpuLimit = 4096;
    if (videoMemory >= 23500)
    {
        gpuLimit = 65536;
    }
    else if (videoMemory >= 15500)
    {
        gpuLimit = 32768;
    }
    else if (videoMemory >= 11500)
    {
        gpuLimit = 16384;
    }
    // An 8-GiB adapter can run the 8B Q4 model well, but an 8192-token KV cache plus
    // normal desktop VRAM growth left too little transient allocation room. Keep this
    // tier latency-first; larger adapters still scale their context automatically.

    int memoryLimit = 4096;
    if (systemMemory >= 60000)
    {
        memoryLimit = 65536;
    }
    else if (systemMemory >= 30000)
    {
        memoryLimit = 32768;
    }
    else if (systemMemory >= 15000)
    {
        memoryLimit = 8192;
    }
    return std::min(gpuLimit, memoryLimit);
}
#endif

} // namespace

llamaHardwareMemory DetectLlamaHardwareMemory()
{
#ifdef _WIN32
    return {DedicatedVideoMemoryMiB(), SystemMemoryMiB()};
#else
    return {};
#endif
}

llamaHardwarePlan PlanLlamaHardware(
    const std::uint64_t dedicatedVideoMemoryMiB,
    const std::uint64_t systemMemoryMiB,
    const int reservedVramMiB)
{
    llamaHardwarePlan plan;
#ifdef _WIN32
    plan.contextTokens = AutomaticContextSize(
        dedicatedVideoMemoryMiB,
        systemMemoryMiB);
#else
    (void)dedicatedVideoMemoryMiB;
    (void)systemMemoryMiB;
#endif

    const std::uint64_t reservation = reservedVramMiB > 0
        ? static_cast<std::uint64_t>(reservedVramMiB)
        : 0;
    const std::uint64_t usableVideoMemory = dedicatedVideoMemoryMiB > reservation
        ? dedicatedVideoMemoryMiB - reservation
        : 0;
    if (usableVideoMemory >= 43000 && systemMemoryMiB >= 60000)
    {
        plan.parallelRequests = 3;
    }
    else if (usableVideoMemory >= 19000 && systemMemoryMiB >= 30000)
    {
        plan.parallelRequests = 2;
    }
    return plan;
}

llamaCppServerProcess::~llamaCppServerProcess()
{
#ifdef _WIN32
    if (processHandle == nullptr)
    {
        return;
    }

    if (bShutdownOnExit)
    {
        Stop();
    }
    else
    {
        if (jobHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(jobHandle));
            jobHandle = nullptr;
        }
        CloseHandle(static_cast<HANDLE>(processHandle));
        processHandle = nullptr;
    }
#endif
}

bool llamaCppServerProcess::Start(const llmSettings& settings, std::string& outError)
{
    return StartInternal(settings, false, "", "", outError);
}

bool llamaCppServerProcess::StartEmbedding(
    const embeddingSettings& settings,
    std::string& outError)
{
    llmSettings launchSettings;
    launchSettings.host = settings.host;
    launchSettings.port = settings.port;
    launchSettings.modelName = settings.modelName;
    launchSettings.apiKey = settings.apiKey;
    launchSettings.serverExecutable = settings.serverExecutable;
    launchSettings.modelPath = settings.modelPath;
    launchSettings.contextSize = settings.contextSize;
    launchSettings.parallelRequests = settings.parallelRequests;
    launchSettings.bAutoTune = false;
    launchSettings.bShutdownServerOnExit = settings.bShutdownServerOnExit;
    return StartInternal(
        launchSettings,
        true,
        settings.pooling,
        settings.device,
        outError);
}

bool llamaCppServerProcess::StartInternal(
    const llmSettings& settings,
    const bool embeddingMode,
    const std::string& pooling,
    const std::string& device,
    std::string& outError)
{
    outError.clear();

#ifndef _WIN32
    (void)settings;
    outError = "Automatic llama.cpp startup is currently supported on Windows only.";
    return false;
#else
    if (processHandle != nullptr)
    {
        if (IsRunning())
        {
            outError = "Revia already owns a llama.cpp server process.";
            return false;
        }

        // A crashed child still leaves a valid Windows process handle. Release that
        // stale handle so the same owner can relaunch the server on the next turn.
        Stop();
    }

    const std::filesystem::path executable = ResolveRuntimePath(settings.serverExecutable);
    const std::filesystem::path model = ResolveRuntimePath(settings.modelPath);
    std::error_code pathError;
    if (!std::filesystem::is_regular_file(executable, pathError))
    {
        outError = "llama.cpp server executable was not found: " + settings.serverExecutable;
        return false;
    }
    pathError.clear();
    if (!std::filesystem::is_regular_file(model, pathError))
    {
        outError = "llama.cpp model was not found: " + settings.modelPath;
        return false;
    }

    const std::wstring executableWide = executable.wstring();
    const std::wstring modelWide = model.wstring();
    const std::wstring hostWide = Utf8ToWide(settings.host);
    if (executableWide.empty() || modelWide.empty() || hostWide.empty())
    {
        outError = "llama.cpp startup settings contain a path or host that Windows could not encode.";
        return false;
    }

    const llamaHardwareMemory hardwareMemory = DetectLlamaHardwareMemory();
    const llamaHardwarePlan hardwarePlan = PlanLlamaHardware(
        hardwareMemory.dedicatedVideoMemoryMiB,
        hardwareMemory.systemMemoryMiB,
        settings.reservedVramMiB);
    const int contextSize = !embeddingMode && settings.bAutoTune
        ? hardwarePlan.contextTokens
        : settings.contextSize;
    const int parallelRequests = !embeddingMode && settings.bAutoTune
        ? hardwarePlan.parallelRequests
        : settings.parallelRequests;
    std::wstring commandLine = QuoteWindowsArgument(executableWide) +
        L" --model " + QuoteWindowsArgument(modelWide) +
        L" --host " + QuoteWindowsArgument(hostWide) +
        L" --port " + std::to_wstring(settings.port) +
        L" --ctx-size " + std::to_wstring(contextSize) +
        L" --parallel " + std::to_wstring(parallelRequests);
    if (!settings.apiKey.empty())
    {
        const std::wstring apiKey = Utf8ToWide(settings.apiKey);
        if (apiKey.empty())
        {
            outError = "The local llama.cpp API key could not be encoded by Windows.";
            return false;
        }
        commandLine += L" --api-key " + QuoteWindowsArgument(apiKey);
    }
    if (!embeddingMode)
    {
        if (settings.bVisionEnabled)
        {
            const std::filesystem::path projector =
                ResolveRuntimePath(settings.multimodalProjectorPath);
            if (!std::filesystem::is_regular_file(projector, pathError))
            {
                outError = "The configured multimodal projector was not found: " +
                    settings.multimodalProjectorPath;
                return false;
            }
            const std::filesystem::path media = ResolveRuntimePath(settings.mediaPath);
            pathError.clear();
            std::filesystem::create_directories(media, pathError);
            if (pathError)
            {
                outError = "The configured vision media directory could not be created: " +
                    pathError.message();
                return false;
            }
            commandLine += L" --mmproj " + QuoteWindowsArgument(projector.wstring()) +
                L" --media-path " + QuoteWindowsArgument(media.wstring());
        }
        if (settings.bAutoTune)
        {
            // Anything still waiting to load into VRAM is added to the fit target so
            // llama.cpp leaves room for it rather than claiming it first.
            const int fitTarget = settings.autoFitTargetMiB +
                (settings.reservedVramMiB > 0 ? settings.reservedVramMiB : 0);
            commandLine += L" --n-gpu-layers auto --fit on --fit-target " +
                std::to_wstring(fitTarget) +
                L" --flash-attn auto";
        }
        else
        {
            commandLine += L" --fit off --flash-attn auto";
        }
    }
    if (embeddingMode)
    {
        const std::wstring poolingWide = Utf8ToWide(pooling);
        if (poolingWide.empty())
        {
            outError = "The embedding pooling setting could not be encoded by Windows.";
            return false;
        }
        commandLine += L" --embedding --pooling " + QuoteWindowsArgument(poolingWide);
        const std::wstring deviceWide = Utf8ToWide(device);
        if (deviceWide.empty())
        {
            outError = "The embedding device setting could not be encoded by Windows.";
            return false;
        }
        commandLine += L" --device " + QuoteWindowsArgument(deviceWide);
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::filesystem::create_directories("Logs", pathError);
    if (pathError)
    {
        outError = "Could not create the llama.cpp log directory: " + pathError.message();
        return false;
    }

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    const wchar_t* stdoutPath = embeddingMode
        ? L"Logs\\embedding-server.stdout.log"
        : L"Logs\\llama-server.stdout.log";
    const wchar_t* stderrPath = embeddingMode
        ? L"Logs\\embedding-server.stderr.log"
        : L"Logs\\llama-server.stderr.log";

    const HANDLE standardOutput = CreateFileW(
        stdoutPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (standardOutput == INVALID_HANDLE_VALUE)
    {
        outError = WindowsError("Opening the llama.cpp stdout log");
        return false;
    }

    const HANDLE standardError = CreateFileW(
        stderrPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (standardError == INVALID_HANDLE_VALUE)
    {
        outError = WindowsError("Opening the llama.cpp stderr log");
        CloseHandle(standardOutput);
        return false;
    }

    const HANDLE standardInput = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (standardInput == INVALID_HANDLE_VALUE)
    {
        outError = WindowsError("Opening the llama.cpp null input");
        CloseHandle(standardError);
        CloseHandle(standardOutput);
        return false;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = standardInput;
    startupInfo.hStdOutput = standardOutput;
    startupInfo.hStdError = standardError;

    PROCESS_INFORMATION processInformation{};
    const std::wstring workingDirectory = executable.parent_path().wstring();
    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
        (settings.bShutdownServerOnExit ? CREATE_SUSPENDED : 0);
    const BOOL created = CreateProcessW(
        executableWide.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        creationFlags,
        nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startupInfo,
        &processInformation);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

    CloseHandle(standardInput);
    CloseHandle(standardError);
    CloseHandle(standardOutput);

    if (!created)
    {
        SetLastError(createError);
        outError = WindowsError("Starting llama.cpp");
        return false;
    }

    if (settings.bShutdownServerOnExit)
    {
        const HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job != nullptr &&
            SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                &limits,
                sizeof(limits)) &&
            AssignProcessToJobObject(job, processInformation.hProcess))
        {
            jobHandle = job;
        }
        else if (job != nullptr)
        {
            CloseHandle(job);
        }

        if (ResumeThread(processInformation.hThread) == static_cast<DWORD>(-1))
        {
            const DWORD resumeError = GetLastError();
            if (jobHandle != nullptr)
            {
                CloseHandle(static_cast<HANDLE>(jobHandle));
                jobHandle = nullptr;
            }
            TerminateProcess(processInformation.hProcess, 1);
            CloseHandle(processInformation.hThread);
            CloseHandle(processInformation.hProcess);
            SetLastError(resumeError);
            outError = WindowsError("Resuming llama.cpp");
            return false;
        }
    }

    CloseHandle(processInformation.hThread);
    processHandle = processInformation.hProcess;
    bShutdownOnExit = settings.bShutdownServerOnExit;
    return true;
#endif
}

bool llamaCppServerProcess::IsRunning() const
{
#ifndef _WIN32
    return false;
#else
    if (processHandle == nullptr)
    {
        return false;
    }

    DWORD exitCode = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode) &&
        exitCode == STILL_ACTIVE;
#endif
}

bool llamaCppServerProcess::WasStartedByRevia() const
{
#ifdef _WIN32
    return processHandle != nullptr;
#else
    return false;
#endif
}

void llamaCppServerProcess::Stop()
{
#ifdef _WIN32
    if (processHandle == nullptr)
    {
        return;
    }

    const HANDLE handle = static_cast<HANDLE>(processHandle);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(handle, &exitCode) && exitCode == STILL_ACTIVE)
    {
        if (jobHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(jobHandle));
            jobHandle = nullptr;
        }
        else
        {
            TerminateProcess(handle, 0);
        }
        WaitForSingleObject(handle, 1000);
    }
    else if (jobHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(jobHandle));
        jobHandle = nullptr;
    }

    CloseHandle(handle);
    processHandle = nullptr;
#endif
}
