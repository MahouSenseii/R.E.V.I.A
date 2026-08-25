#include "Internet/visibleBrowserProcess.h"

#include "Internet/visibleBrowserClient.h"

#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace revia::actions::internet
{

namespace
{
#ifdef _WIN32
std::wstring QuoteWindowsArgument(const std::wstring& value)
{
    std::wstring quoted = L"\"";
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
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ResolveRuntimePath(const std::filesystem::path& relative)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(relative, error))
        return std::filesystem::absolute(relative, error);
    const std::filesystem::path besideExecutable = ExecutableDirectory() / relative;
    error.clear();
    if (std::filesystem::is_regular_file(besideExecutable, error)) return besideExecutable;
    return {};
}

std::filesystem::path ResolveNode()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(
        nullptr, L"node.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length > 0 && length < buffer.size())
    {
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }

    const wchar_t* programFiles = _wgetenv(L"ProgramFiles");
    if (programFiles != nullptr)
    {
        const std::filesystem::path conventional =
            std::filesystem::path(programFiles) / "nodejs" / "node.exe";
        std::error_code error;
        if (std::filesystem::is_regular_file(conventional, error)) return conventional;
    }
    return {};
}

std::string RandomToken()
{
    std::array<unsigned char, 32> bytes{};
    if (BCryptGenRandom(
            nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    {
        return {};
    }
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes) encoded << std::setw(2) << static_cast<int>(byte);
    return encoded.str();
}

std::string WindowsError(const std::string& operation)
{
    return operation + " failed with Windows error " + std::to_string(GetLastError()) + ".";
}
#endif
} // namespace

VisibleBrowserProcess::~VisibleBrowserProcess()
{
    Stop();
}

bool VisibleBrowserProcess::Start(
    const CapabilitySettings::InternetAccess& settings,
    std::string& outError)
{
    outError.clear();
#ifndef _WIN32
    (void)settings;
    outError = "The visible browser worker is currently supported on Windows only.";
    return false;
#else
    if (IsRunning()) return true;
    Stop();

    const std::filesystem::path node = ResolveNode();
    if (node.empty())
    {
        outError = "Node.js was not found on PATH or under Program Files\\nodejs.";
        return false;
    }
    const std::filesystem::path worker = ResolveRuntimePath("Tools/Browser/browserHost.mjs");
    if (worker.empty())
    {
        outError = "The project-owned visible browser worker was not found at Tools/Browser/browserHost.mjs.";
        return false;
    }

    token = RandomToken();
    port = settings.visibleBrowserPort;
    if (token.empty() || port < 1024 || port > 65535)
    {
        outError = "The visible browser service could not create a valid private endpoint.";
        token.clear();
        port = 0;
        return false;
    }

    std::error_code error;
    const std::filesystem::path profile =
        std::filesystem::absolute("RuntimeData/Browser/Profile", error);
    if (error || profile.empty())
    {
        outError = "The dedicated visible browser profile path could not be resolved.";
        token.clear();
        port = 0;
        return false;
    }
    std::filesystem::create_directories(profile, error);
    std::filesystem::create_directories("Logs", error);
    if (error)
    {
        outError = "The visible browser runtime directories could not be created: " + error.message();
        token.clear();
        port = 0;
        return false;
    }

    const std::wstring commandLine = QuoteWindowsArgument(node.wstring()) + L" " +
        QuoteWindowsArgument(worker.wstring()) + L" --host 127.0.0.1 --port " +
        std::to_wstring(port) + L" --token " + QuoteWindowsArgument(
            std::wstring(token.begin(), token.end())) + L" --profile " +
        QuoteWindowsArgument(profile.wstring()) + L" --max-pages " +
        std::to_wstring(settings.visibleBrowserMaxPages) + L" --step-delay-ms " +
        std::to_wstring(settings.visibleBrowserStepDelayMs);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const HANDLE output = CreateFileW(
        L"Logs\\visible-browser.stdout.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE errors = CreateFileW(
        L"Logs\\visible-browser.stderr.log", FILE_APPEND_DATA,
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
        outError = WindowsError("Opening visible browser worker logs");
        token.clear();
        port = 0;
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = errors;
    PROCESS_INFORMATION information{};
    const BOOL created = CreateProcessW(
        node.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        nullptr, worker.parent_path().c_str(), &startup, &information);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(input);
    CloseHandle(errors);
    CloseHandle(output);
    if (!created)
    {
        SetLastError(createError);
        outError = WindowsError("Starting the visible browser worker");
        token.clear();
        port = 0;
        return false;
    }

    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job == nullptr || !SetInformationJobObject(
            job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, information.hProcess))
    {
        const DWORD jobError = GetLastError();
        if (job != nullptr) CloseHandle(job);
        TerminateProcess(information.hProcess, 1);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
        SetLastError(jobError);
        outError = WindowsError("Containing the visible browser worker");
        token.clear();
        port = 0;
        return false;
    }
    jobHandle = job;
    if (ResumeThread(information.hThread) == static_cast<DWORD>(-1))
    {
        const DWORD resumeError = GetLastError();
        CloseHandle(static_cast<HANDLE>(jobHandle));
        jobHandle = nullptr;
        TerminateProcess(information.hProcess, 1);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
        SetLastError(resumeError);
        outError = WindowsError("Resuming the visible browser worker");
        token.clear();
        port = 0;
        return false;
    }
    CloseHandle(information.hThread);
    processHandle = information.hProcess;
    return true;
#endif
}

bool VisibleBrowserProcess::IsRunning() const
{
#ifndef _WIN32
    return false;
#else
    if (processHandle == nullptr) return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode) &&
        exitCode == STILL_ACTIVE;
#endif
}

void VisibleBrowserProcess::Stop()
{
#ifdef _WIN32
    if (processHandle == nullptr)
    {
        if (jobHandle != nullptr) CloseHandle(static_cast<HANDLE>(jobHandle));
        jobHandle = nullptr;
        token.clear();
        port = 0;
        return;
    }

    const HANDLE process = static_cast<HANDLE>(processHandle);
    if (IsRunning() && port > 0 && !token.empty())
    {
        VisibleBrowserClient(port, token).RequestShutdown();
        WaitForSingleObject(process, 3000);
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE)
    {
        if (jobHandle != nullptr) TerminateJobObject(static_cast<HANDLE>(jobHandle), 0);
        else TerminateProcess(process, 0);
        WaitForSingleObject(process, 2000);
    }
    if (jobHandle != nullptr) CloseHandle(static_cast<HANDLE>(jobHandle));
    CloseHandle(process);
    processHandle = nullptr;
    jobHandle = nullptr;
#endif
    token.clear();
    port = 0;
}

int VisibleBrowserProcess::Port() const
{
    return port;
}

const std::string& VisibleBrowserProcess::Token() const
{
    return token;
}

} // namespace revia::actions::internet
