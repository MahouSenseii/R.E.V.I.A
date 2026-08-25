#include "Speech/whisperServerProcess.h"

#include "Speech/speechRecognitionService.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::speech
{

namespace
{
#ifdef _WIN32
    std::wstring Quote(const std::wstring& value)
    {
        if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
        std::wstring output = L"\"";
        std::size_t slashes = 0;
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++slashes;
                continue;
            }
            if (character == L'\"')
            {
                output.append(slashes * 2 + 1, L'\\');
                output.push_back(character);
                slashes = 0;
                continue;
            }
            output.append(slashes, L'\\');
            slashes = 0;
            output.push_back(character);
        }
        output.append(slashes * 2, L'\\');
        output.push_back(L'\"');
        return output;
    }

    std::wstring Wide(const std::string& value)
    {
        if (value.empty()) return {};
        const int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            nullptr, 0);
        if (count <= 0) return {};
        std::wstring output(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), count);
        return output;
    }

    std::optional<int> CudaOrdinal(std::string device)
    {
        std::transform(device.begin(), device.end(), device.begin(),
            [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (!device.starts_with("cuda:") || device.size() <= 5) return std::nullopt;
        try
        {
            std::size_t consumed = 0;
            const std::string digits = device.substr(5);
            const int value = std::stoi(digits, &consumed);
            return value >= 0 && consumed == digits.size()
                ? std::optional<int>(value) : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
#endif
}

WhisperServerProcess::~WhisperServerProcess()
{
    Stop();
}

bool WhisperServerProcess::Start(
    const speechRecognitionSettings& settings,
    std::string& outError)
{
    std::lock_guard lock(processMutex);
    outError.clear();
#ifndef _WIN32
    (void)settings;
    outError = "The warm whisper.cpp server is currently supported on Windows only.";
    return false;
#else
    if (processHandle != nullptr)
    {
        if (IsRunningLocked()) return true;
        StopLocked();
    }
    const std::filesystem::path executable =
        SpeechRecognitionService::ResolveRuntimePath(settings.serverExecutable);
    const std::filesystem::path model =
        SpeechRecognitionService::ResolveRuntimePath(settings.modelPath);
    if (!std::filesystem::is_regular_file(executable) ||
        !std::filesystem::is_regular_file(model))
    {
        outError = "whisper-server.exe or its model is missing.";
        return false;
    }
    std::wstring command = Quote(executable.wstring()) + L" -m " + Quote(model.wstring()) +
        L" --host " + Quote(Wide(settings.serverHost)) + L" --port " +
        std::to_wstring(settings.serverPort) + L" -l " + Quote(Wide(settings.language)) +
        L" -t " + std::to_wstring(settings.threads) + L" --no-timestamps";
    const std::optional<int> device = CudaOrdinal(settings.device);
    if (!settings.bUseGpu || settings.device == "cpu") command += L" -ng";
    else if (device.has_value()) command += L" -dev " + std::to_wstring(*device);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    std::error_code error;
    std::filesystem::create_directories("Logs", error);
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const HANDLE output = CreateFileW(
        L"Logs\\whisper-server.stdout.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE errors = CreateFileW(
        L"Logs\\whisper-server.stderr.log", FILE_APPEND_DATA,
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
        outError = "The whisper.cpp log files could not be opened.";
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = errors;
    PROCESS_INFORMATION information{};
    const std::wstring workingDirectory = executable.parent_path().wstring();
    const BOOL created = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        nullptr, workingDirectory.c_str(), &startup, &information);
    CloseHandle(input);
    CloseHandle(errors);
    CloseHandle(output);
    if (!created)
    {
        outError = "Starting whisper-server.exe failed with Windows error " +
            std::to_string(GetLastError()) + ".";
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
        TerminateProcess(information.hProcess, 1);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
        if (jobHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(jobHandle));
            jobHandle = nullptr;
        }
        outError = "The whisper.cpp server thread could not be resumed.";
        return false;
    }
    CloseHandle(information.hThread);
    processHandle = information.hProcess;
    return true;
#endif
}

bool WhisperServerProcess::IsRunning() const
{
    std::lock_guard lock(processMutex);
    return IsRunningLocked();
}

bool WhisperServerProcess::IsRunningLocked() const
{
#ifndef _WIN32
    return false;
#else
    if (processHandle == nullptr) return false;
    DWORD code = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(processHandle), &code) &&
        code == STILL_ACTIVE;
#endif
}

void WhisperServerProcess::Stop()
{
    std::lock_guard lock(processMutex);
    StopLocked();
}

void WhisperServerProcess::StopLocked()
{
#ifdef _WIN32
    if (processHandle == nullptr) return;
    const HANDLE process = static_cast<HANDLE>(processHandle);
    if (jobHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(jobHandle));
        jobHandle = nullptr;
    }
    else
    {
        TerminateProcess(process, 0);
    }
    WaitForSingleObject(process, 3000);
    CloseHandle(process);
    processHandle = nullptr;
#endif
}

} // namespace revia::speech
