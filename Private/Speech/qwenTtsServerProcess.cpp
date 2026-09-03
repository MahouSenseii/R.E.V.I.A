#include "Speech/qwenTtsServerProcess.h"

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::speech
{

namespace
{
#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
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
        const auto executableDirectory =
            std::filesystem::path(std::wstring(module.data(), length)).parent_path();
        for (const auto& root : {
            executableDirectory,
            executableDirectory.parent_path(),
            executableDirectory.parent_path().parent_path()})
        {
            const auto candidate = (root / value).lexically_normal();
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }
    }
    return current;
}

std::string WindowsError(const std::string& operation)
{
    return operation + " failed with Windows error " + std::to_string(GetLastError()) + ".";
}
#endif
}

QwenTtsServerProcess::~QwenTtsServerProcess()
{
    Stop();
}

bool QwenTtsServerProcess::Start(
    const speechSettings& settings,
    const std::string& apiKey,
    std::string& outError)
{
    outError.clear();
#ifndef _WIN32
    (void)settings;
    (void)apiKey;
    outError = "Automatic Qwen3-TTS startup is currently supported on Windows only.";
    return false;
#else
    if (processHandle != nullptr)
    {
        return IsRunning();
    }
    const std::filesystem::path script = ResolveRuntimePath(settings.qwenServiceScript);
    std::error_code error;
    if (!std::filesystem::is_regular_file(script, error))
    {
        outError = "Qwen3-TTS service script was not found: " + settings.qwenServiceScript;
        return false;
    }

    const std::filesystem::path resolvedPython = ResolveRuntimePath(settings.pythonExecutable);
    const std::wstring python = std::filesystem::is_regular_file(resolvedPython, error)
        ? resolvedPython.wstring()
        : Utf8ToWide(settings.pythonExecutable);
    const std::wstring token = Utf8ToWide(apiKey);
    if (python.empty() || token.empty())
    {
        outError = "The Qwen3-TTS Python executable or API key could not be encoded.";
        return false;
    }
    std::wstring commandLine = QuoteWindowsArgument(python) + L" " +
        QuoteWindowsArgument(script.wstring()) +
        L" --host " + QuoteWindowsArgument(Utf8ToWide(settings.qwenHost)) +
        L" --port " + std::to_wstring(settings.qwenPort) +
        L" --token " + QuoteWindowsArgument(token) +
        L" --device " + QuoteWindowsArgument(Utf8ToWide(settings.qwenDevice)) +
        L" --minimum-free-vram-mib " + std::to_wstring(settings.qwenMinimumFreeVramMiB) +
        L" --cpu-threads " + std::to_wstring(settings.qwenCpuThreads) +
        L" --max-audio-mib " + std::to_wstring(settings.qwenMaxBufferedAudioMiB) +
        L" --design-model " + QuoteWindowsArgument(Utf8ToWide(settings.qwenVoiceDesignModel)) +
        L" --clone-model " + QuoteWindowsArgument(Utf8ToWide(settings.qwenCloneModel)) +
        L" --attention-backend " +
            QuoteWindowsArgument(Utf8ToWide(settings.qwenAttentionBackend)) +
        L" --input-mode " + QuoteWindowsArgument(Utf8ToWide(settings.qwenInputMode));
    // Passed as flags rather than values so an older worker script that does not know
    // them fails loudly at startup instead of quietly ignoring a value it parsed.
    if (settings.bQwenLowLatencyPhrase)
    {
        commandLine += L" --low-latency";
        if (settings.bQwenCudaGraph)
        {
            commandLine += L" --cuda-graph";
            // Stage 2 only means anything with the predictor graph already on.
            if (settings.bQwenTalkerGraph)
            {
                commandLine += L" --talker-graph";
            }
        }
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::filesystem::create_directories("Logs", error);
    if (error)
    {
        outError = "Could not create the Qwen3-TTS log directory: " + error.message();
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const std::wstring logSuffix = L"-" + std::to_wstring(settings.qwenPort);
    const std::wstring stdoutPath = L"Logs\\qwen-tts" + logSuffix + L".stdout.log";
    const std::wstring stderrPath = L"Logs\\qwen-tts" + logSuffix + L".stderr.log";
    const HANDLE output = CreateFileW(
        stdoutPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE errors = CreateFileW(
        stderrPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE || errors == INVALID_HANDLE_VALUE ||
        input == INVALID_HANDLE_VALUE)
    {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (errors != INVALID_HANDLE_VALUE) CloseHandle(errors);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        outError = WindowsError("Opening Qwen3-TTS process logs");
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = errors;
    PROCESS_INFORMATION information{};
    const std::wstring workingDirectory = script.parent_path().wstring();
    const BOOL created = CreateProcessW(
        nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        nullptr, workingDirectory.c_str(), &startup, &information);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(input);
    CloseHandle(errors);
    CloseHandle(output);
    if (!created)
    {
        SetLastError(createError);
        outError = WindowsError("Starting Qwen3-TTS");
        return false;
    }

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
        outError = WindowsError("Resuming Qwen3-TTS");
        return false;
    }
    CloseHandle(information.hThread);
    processHandle = information.hProcess;
    return true;
#endif
}

bool QwenTtsServerProcess::IsRunning() const
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

bool QwenTtsServerProcess::WasStartedByRevia() const
{
#ifdef _WIN32
    return processHandle != nullptr;
#else
    return false;
#endif
}

void QwenTtsServerProcess::Stop()
{
#ifdef _WIN32
    if (processHandle == nullptr)
    {
        return;
    }
    const HANDLE handle = static_cast<HANDLE>(processHandle);
    if (jobHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(jobHandle));
        jobHandle = nullptr;
    }
    else
    {
        TerminateProcess(handle, 0);
    }
    WaitForSingleObject(handle, 2000);
    CloseHandle(handle);
    processHandle = nullptr;
#endif
}

} // namespace revia::speech
