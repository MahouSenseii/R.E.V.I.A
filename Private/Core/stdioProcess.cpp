#include "Core/stdioProcess.h"

#include "Core/logger.h"
#include "Core/runtimePath.h"

#include <algorithm>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace revia::core
{

namespace
{
constexpr std::size_t ReadChunk = 65536;

std::filesystem::path ResolveCommand(const std::string& command)
{
    std::error_code error;
    const std::filesystem::path resolved = ResolveRuntimePath(std::filesystem::path(command));
    if (std::filesystem::is_regular_file(resolved, error)) return resolved;
    return std::filesystem::path(command);
}

std::filesystem::path StderrLog(const std::string& logName)
{
    std::error_code error;
    std::filesystem::create_directories(ReviaLogDirectory(), error);
    return std::filesystem::path(ReviaLogDirectory()) / (logName + ".stderr.log");
}

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
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
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

void CloseIf(void*& handle)
{
    if (handle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}
#endif
} // namespace

StdioProcess::~StdioProcess()
{
    Stop();
}

bool StdioProcess::Start(const StdioLaunch& launch, std::string& outError)
{
    outError.clear();
    if (IsRunning())
    {
        outError = "The " + launch.displayName + " is already running.";
        return false;
    }
    Stop();
    pending.clear();
    exitCode = -1;
    const std::filesystem::path command = ResolveCommand(launch.command);
    if (launch.command.empty())
    {
        outError = "No command is configured for the " + launch.displayName + ".";
        return false;
    }
    std::error_code error;
    if (!launch.workingDirectory.empty() &&
        !std::filesystem::is_directory(launch.workingDirectory, error))
    {
        outError = "The " + launch.displayName + " working directory does not exist: " +
            launch.workingDirectory.string();
        return false;
    }
    const std::filesystem::path errorLog = StderrLog(launch.logName);
#ifdef _WIN32
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE childStdinRead = nullptr;
    HANDLE parentStdinWrite = nullptr;
    HANDLE childStdoutWrite = nullptr;
    HANDLE parentStdoutRead = nullptr;
    if (!CreatePipe(&childStdinRead, &parentStdinWrite, &inheritable, 0) ||
        !CreatePipe(&parentStdoutRead, &childStdoutWrite, &inheritable, 0))
    {
        outError = WindowsError("Creating the " + launch.displayName + " pipes");
        return false;
    }
    // The parent's ends must not leak into the child, or its stdout never closes.
    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);
    const HANDLE errors = CreateFileW(errorLog.wstring().c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errors == INVALID_HANDLE_VALUE)
    {
        outError = WindowsError("Opening the " + launch.displayName + " log");
        CloseHandle(childStdinRead);
        CloseHandle(parentStdinWrite);
        CloseHandle(childStdoutWrite);
        CloseHandle(parentStdoutRead);
        return false;
    }

    std::wstring commandLine = QuoteArgument(command.wstring());
    for (const std::string& argument : launch.arguments)
    {
        commandLine += L" " + QuoteArgument(Widen(argument));
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdinRead;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = errors;
    PROCESS_INFORMATION information{};
    const std::wstring workingDirectory = launch.workingDirectory.wstring();
    const BOOL created = CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &information);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);
    CloseHandle(errors);
    if (!created)
    {
        CloseHandle(parentStdinWrite);
        CloseHandle(parentStdoutRead);
        SetLastError(createError);
        outError = WindowsError("Starting the " + launch.displayName);
        return false;
    }
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (launch.memoryLimitMiB > 0)
    {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(launch.memoryLimitMiB * 1024ULL * 1024ULL);
    }
    if (launch.cpuSecondsLimit > 0)
    {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
        limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart =
            static_cast<LONGLONG>(launch.cpuSecondsLimit) * 10000000LL;
    }
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
        CloseIf(jobHandle);
        TerminateProcess(information.hProcess, 1);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
        CloseHandle(parentStdinWrite);
        CloseHandle(parentStdoutRead);
        outError = WindowsError("Resuming the " + launch.displayName);
        return false;
    }
    CloseHandle(information.hThread);
    processHandle = information.hProcess;
    stdinWrite = parentStdinWrite;
    stdoutRead = parentStdoutRead;
    return true;
#else
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0)
    {
        outError = "Creating the " + launch.displayName + " pipes failed: " + std::strerror(errno);
        if (inPipe[0] >= 0) { close(inPipe[0]); close(inPipe[1]); }
        return false;
    }
    std::vector<std::string> argumentStorage;
    argumentStorage.push_back(command.string());
    for (const std::string& argument : launch.arguments) argumentStorage.push_back(argument);
    std::vector<char*> argv;
    for (std::string& argument : argumentStorage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const std::string errorLogText = errorLog.string();
    const std::string workingDirectory = launch.workingDirectory.string();

    const pid_t child = fork();
    if (child < 0)
    {
        outError = "Starting the " + launch.displayName + " failed: " + std::strerror(errno);
        close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
        return false;
    }
    if (child == 0)
    {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        const int errors = open(errorLogText.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (errors >= 0) { dup2(errors, STDERR_FILENO); close(errors); }
        close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]);
        if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0) _exit(126);
        if (launch.memoryLimitMiB > 0)
        {
            rlimit memory{};
            memory.rlim_cur = memory.rlim_max =
                static_cast<rlim_t>(launch.memoryLimitMiB) * 1024ULL * 1024ULL;
            (void)setrlimit(RLIMIT_AS, &memory);
        }
        if (launch.cpuSecondsLimit > 0)
        {
            rlimit cpu{};
            cpu.rlim_cur = cpu.rlim_max = static_cast<rlim_t>(launch.cpuSecondsLimit);
            (void)setrlimit(RLIMIT_CPU, &cpu);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    fcntl(inPipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);
    pid = child;
    stdinWrite = inPipe[1];
    stdoutRead = outPipe[0];
    return true;
#endif
}

bool StdioProcess::IsRunning() const
{
#ifdef _WIN32
    if (processHandle == nullptr) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(processHandle), &code)) return false;
    if (code == STILL_ACTIVE) return true;
    const_cast<StdioProcess*>(this)->exitCode = static_cast<int>(code);
    return false;
#else
    if (pid <= 0) return false;
    int status = 0;
    const pid_t reaped = waitpid(pid, &status, WNOHANG);
    if (reaped == 0) return true;
    if (reaped == pid)
    {
        StdioProcess* self = const_cast<StdioProcess*>(this);
        self->exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
            : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        self->pid = -1;
    }
    return false;
#endif
}

void StdioProcess::CloseInput()
{
#ifdef _WIN32
    CloseIf(stdinWrite);
#else
    if (stdinWrite >= 0)
    {
        close(stdinWrite);
        stdinWrite = -1;
    }
#endif
}

int StdioProcess::ExitCode() const
{
    (void)IsRunning();
    return exitCode;
}

bool StdioProcess::Write(const std::string& bytes)
{
#ifdef _WIN32
    if (stdinWrite == nullptr) return false;
    std::size_t written = 0;
    while (written < bytes.size())
    {
        DWORD count = 0;
        if (!WriteFile(static_cast<HANDLE>(stdinWrite), bytes.data() + written,
                static_cast<DWORD>(bytes.size() - written), &count, nullptr))
        {
            return false;
        }
        written += count;
    }
    return true;
#else
    if (stdinWrite < 0) return false;
    std::size_t written = 0;
    while (written < bytes.size())
    {
        const ssize_t count = write(stdinWrite, bytes.data() + written, bytes.size() - written);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
#endif
}

bool StdioProcess::Fill(const std::chrono::milliseconds timeout, bool& outClosed)
{
    outClosed = false;
#ifdef _WIN32
    if (stdoutRead == nullptr)
    {
        outClosed = true;
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(stdoutRead), nullptr, 0, nullptr, &available, nullptr))
        {
            outClosed = true;
            return false;
        }
        if (available > 0) break;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(10);
    }
    char buffer[ReadChunk];
    DWORD count = 0;
    if (!ReadFile(static_cast<HANDLE>(stdoutRead), buffer, sizeof(buffer), &count, nullptr) || count == 0)
    {
        outClosed = true;
        return false;
    }
    pending.append(buffer, count);
    return true;
#else
    if (stdoutRead < 0)
    {
        outClosed = true;
        return false;
    }
    pollfd waiting{};
    waiting.fd = stdoutRead;
    waiting.events = POLLIN;
    const int ready = poll(&waiting, 1, static_cast<int>(std::max<long long>(0, timeout.count())));
    if (ready == 0) return false;
    if (ready < 0)
    {
        if (errno == EINTR) return false;
        outClosed = true;
        return false;
    }
    char buffer[ReadChunk];
    const ssize_t count = read(stdoutRead, buffer, sizeof(buffer));
    if (count <= 0)
    {
        outClosed = true;
        return false;
    }
    pending.append(buffer, static_cast<std::size_t>(count));
    return true;
#endif
}

bool StdioProcess::ReadLine(std::string& outLine, const std::chrono::milliseconds timeout, bool& outClosed)
{
    outClosed = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        const std::size_t newline = pending.find('\n');
        if (newline != std::string::npos)
        {
            outLine = pending.substr(0, newline);
            if (!outLine.empty() && outLine.back() == '\r') outLine.pop_back();
            pending.erase(0, newline + 1);
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        bool closed = false;
        if (!Fill(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), closed))
        {
            if (closed)
            {
                outClosed = true;
                // A last line without its newline still counts.
                if (!pending.empty())
                {
                    outLine = pending;
                    pending.clear();
                    return true;
                }
            }
            return false;
        }
    }
}

void StdioProcess::Stop()
{
#ifdef _WIN32
    CloseIf(stdinWrite);
    if (processHandle != nullptr)
    {
        const HANDLE handle = static_cast<HANDLE>(processHandle);
        if (jobHandle != nullptr)
        {
            CloseIf(jobHandle);
        }
        else
        {
            TerminateProcess(handle, 0);
        }
        WaitForSingleObject(handle, 2000);
        DWORD code = 0;
        if (GetExitCodeProcess(handle, &code) && code != STILL_ACTIVE) exitCode = static_cast<int>(code);
        CloseHandle(handle);
        processHandle = nullptr;
    }
    CloseIf(stdoutRead);
#else
    if (stdinWrite >= 0)
    {
        close(stdinWrite);
        stdinWrite = -1;
    }
    if (pid > 0)
    {
        int status = 0;
        pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == 0)
        {
            kill(pid, SIGTERM);
            for (int slice = 0; slice < 200; ++slice)
            {
                reaped = waitpid(pid, &status, WNOHANG);
                if (reaped != 0) break;
                usleep(10000);
            }
            if (reaped == 0)
            {
                kill(pid, SIGKILL);
                reaped = waitpid(pid, &status, 0);
            }
        }
        if (reaped == pid)
        {
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        }
        pid = -1;
    }
    if (stdoutRead >= 0)
    {
        close(stdoutRead);
        stdoutRead = -1;
    }
#endif
    pending.clear();
}

} // namespace revia::core
