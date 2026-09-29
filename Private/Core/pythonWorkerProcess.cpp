#include "Core/pythonWorkerProcess.h"

#include "Core/logger.h"
#include "Core/runtimePath.h"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::core
{

namespace
{
#ifdef _WIN32
std::wstring Widen(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), length);
    return output;
}

std::wstring QuoteArgument(const std::wstring& argument)
{
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
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

std::string WindowsError(const std::string& operation)
{
    return operation + " failed with Windows error " + std::to_string(GetLastError()) + ".";
}
#endif
} // namespace

PythonWorkerProcess::~PythonWorkerProcess()
{
    Stop();
}

bool PythonWorkerProcess::Start(const PythonWorkerLaunch& launch, std::string& outError)
{
    outError.clear();
#ifndef _WIN32
    (void)launch;
    outError = "Starting the " + launch.displayName + " is supported on Windows only.";
    return false;
#else
    if (processHandle != nullptr)
    {
        if (IsRunning()) return true;
        // A dead worker's handle must not block the next start.
        Stop();
    }
    const std::filesystem::path script = ResolveRuntimePath(std::filesystem::path(launch.script));
    std::error_code error;
    if (!std::filesystem::is_regular_file(script, error))
    {
        outError = launch.displayName + " script was not found: " + launch.script;
        return false;
    }
    const std::filesystem::path resolvedPython =
        ResolveRuntimePath(std::filesystem::path(launch.pythonExecutable));
    const std::wstring python = std::filesystem::is_regular_file(resolvedPython, error)
        ? resolvedPython.wstring() : Widen(launch.pythonExecutable);
    if (python.empty())
    {
        outError = "The " + launch.displayName + " Python executable could not be encoded.";
        return false;
    }
    std::wstring commandLine = QuoteArgument(python) + L" " + QuoteArgument(script.wstring());
    for (const std::string& argument : launch.arguments)
    {
        commandLine += L" " + QuoteArgument(Widen(argument));
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::filesystem::create_directories(ReviaLogDirectory(), error);
    if (error)
    {
        outError = "Could not create the log directory: " + error.message();
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const std::wstring stem = (std::filesystem::path(ReviaLogDirectory()) / Widen(launch.logName)).wstring() +
        L"-" + std::to_wstring(launch.port);
    const HANDLE output = CreateFileW((stem + L".stdout.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE errors = CreateFileW((stem + L".stderr.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE || errors == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE)
    {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (errors != INVALID_HANDLE_VALUE) CloseHandle(errors);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        outError = WindowsError("Opening the " + launch.displayName + " logs");
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
    const BOOL created = CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        nullptr, workingDirectory.c_str(), &startup, &information);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(input);
    CloseHandle(errors);
    CloseHandle(output);
    if (!created)
    {
        SetLastError(createError);
        outError = WindowsError("Starting the " + launch.displayName);
        return false;
    }
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job != nullptr && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
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
        outError = WindowsError("Resuming the " + launch.displayName);
        return false;
    }
    CloseHandle(information.hThread);
    processHandle = information.hProcess;
    return true;
#endif
}

bool PythonWorkerProcess::IsRunning() const
{
#ifndef _WIN32
    return false;
#else
    if (processHandle == nullptr) return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode) && exitCode == STILL_ACTIVE;
#endif
}

void PythonWorkerProcess::Stop()
{
#ifdef _WIN32
    if (processHandle == nullptr) return;
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

} // namespace revia::core
