#include "Process/ownedProcess.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include "windowsChildSupport.h"

#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace revia::process
{

struct OwnedProcess::Impl
{
    mutable std::mutex mutex;
    std::condition_variable arrived;
    std::string output;
    std::string errors;
    std::string stopReason;
    bool outputClosed = true;
    bool started = false;
    std::atomic<bool> stopping{false};
    std::size_t maxPendingOutput = 0;
    std::size_t maxErrors = 0;
    std::thread outputReader;
    std::thread errorReader;
#ifdef _WIN32
    windows::Handle job;
    windows::Handle child;
    windows::Handle input;
    windows::Handle outputRead;
    windows::Handle errorRead;
#else
    pid_t child = -1;
    int input = -1;
    int outputRead = -1;
    int errorRead = -1;
#endif

    void AppendOutput(const char* bytes, const std::size_t count)
    {
        bool overflow = false;
        {
            std::lock_guard lock(mutex);
            if (output.size() + count > maxPendingOutput)
            {
                overflow = true;
                if (stopReason.empty())
                    stopReason = "The server wrote more unread output than Revia keeps, so it was stopped.";
            }
            else
            {
                output.append(bytes, count);
            }
        }
        arrived.notify_all();
        if (overflow)
            Kill();
    }

    void AppendError(const char* bytes, const std::size_t count)
    {
        std::lock_guard lock(mutex);
        errors.append(bytes, count);
        if (errors.size() > maxErrors)
            errors.erase(0, errors.size() - maxErrors);
    }

    void CloseOutput()
    {
        {
            std::lock_guard lock(mutex);
            outputClosed = true;
        }
        arrived.notify_all();
    }

#ifdef _WIN32
    // Polling rather than blocking reads: a blocked ReadFile on an anonymous pipe cannot
    // be cancelled portably from MinGW's threads, and a stop must always return.
    void Drain(HANDLE pipe, const bool isOutput)
    {
        std::array<char, 4096> bytes{};
        while (!stopping.load())
        {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                break;
            if (available == 0)
            {
                // Once the child has exited the wait returns at once; a descendant still
                // holding the pipe must not turn this into a spin.
                if (WaitForSingleObject(child.Get(), 10) == WAIT_OBJECT_0)
                    Sleep(10);
                continue;
            }
            DWORD read = 0;
            if (!ReadFile(pipe, bytes.data(), std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &read, nullptr) || read == 0)
                break;
            isOutput ? AppendOutput(bytes.data(), read) : AppendError(bytes.data(), read);
        }
        if (isOutput)
            CloseOutput();
    }

    void Kill()
    {
        if (job.Valid())
            TerminateJobObject(job.Get(), 1);
    }
#else
    void Drain(const int pipe, const bool isOutput)
    {
        std::array<char, 4096> bytes{};
        while (!stopping.load())
        {
            pollfd waiting{pipe, POLLIN, 0};
            const int ready = poll(&waiting, 1, 50);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0)
                break;
            if (ready == 0)
                continue;
            const ssize_t read = ::read(pipe, bytes.data(), bytes.size());
            if (read < 0 && errno == EINTR)
                continue;
            if (read <= 0)
                break;
            isOutput ? AppendOutput(bytes.data(), static_cast<std::size_t>(read)) : AppendError(bytes.data(), static_cast<std::size_t>(read));
        }
        if (isOutput)
            CloseOutput();
    }

    void Kill()
    {
        if (child > 0)
        {
            kill(-child, SIGKILL);
            kill(child, SIGKILL);
        }
    }
#endif
};

OwnedProcess::OwnedProcess() : impl(std::make_unique<Impl>())
{
}

OwnedProcess::~OwnedProcess()
{
    Terminate();
}

bool OwnedProcess::Start(const OwnedProcessRequest& request, std::string& error)
{
    if (impl->started)
    {
        error = "This process owner already started a child.";
        return false;
    }
    if (!request.executable.is_absolute())
    {
        error = "The server command must be an absolute path.";
        return false;
    }
    for (const auto& argument : request.arguments)
    {
        if (argument.find('\0') != std::string::npos || !utf8::IsValid(argument))
        {
            error = "Server arguments must be valid text without NUL characters.";
            return false;
        }
    }
    impl->maxPendingOutput = std::max<std::size_t>(request.maxPendingOutputBytes, 4096);
    impl->maxErrors = std::max<std::size_t>(request.maxErrorBytes, 256);
    const std::filesystem::path directory =
        request.workingDirectory.empty() ? request.executable.parent_path() : request.workingDirectory;
#ifdef _WIN32
    using windows::Handle;
    std::wstring command = windows::Quote(request.executable.native());
    for (const auto& argument : request.arguments)
        command += L" " + windows::Quote(windows::Wide(argument));
    if (command.size() >= 32767)
    {
        error = "The server's arguments exceed Windows' command-line limit.";
        return false;
    }
    auto environment = windows::MinimalEnvironment(request.environment);
    Handle inputRead, inputWrite, outputRead, outputWrite, errorRead, errorWrite;
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.Valid() || !SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !windows::Pipe(inputRead, inputWrite, false) || !windows::Pipe(outputRead, outputWrite, true) ||
        !windows::Pipe(errorRead, errorWrite, true))
    {
        error = "Could not establish the server's lifetime or its standard handles.";
        return false;
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> storage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
    {
        error = "Could not initialize the server's explicit handle list.";
        return false;
    }
    const std::array<HANDLE, 3> inherited{inputRead.Get(), outputWrite.Get(), errorWrite.Get()};
    const bool handlesSet = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inputRead.Get();
    startup.StartupInfo.hStdOutput = outputWrite.Get();
    startup.StartupInfo.hStdError = errorWrite.Get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information{};
    const bool created = handlesSet && CreateProcessW(request.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                           CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                           environment.data(), directory.c_str(), &startup.StartupInfo, &information);
    const DWORD creationError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created)
    {
        error = "Could not start the server (Windows error " + std::to_string(creationError) + ").";
        return false;
    }
    Handle child(information.hProcess), thread(information.hThread);
    inputRead.Reset();
    outputWrite.Reset();
    errorWrite.Reset();
    if (!AssignProcessToJobObject(job.Get(), child.Get()))
    {
        TerminateProcess(child.Get(), 1);
        WaitForSingleObject(child.Get(), INFINITE);
        error = "Process tree ownership could not be established; the suspended server was terminated.";
        return false;
    }
    if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
    {
        TerminateJobObject(job.Get(), 1);
        WaitForSingleObject(child.Get(), INFINITE);
        error = "Could not resume the owned server.";
        return false;
    }
    impl->job.Reset(job.Release());
    impl->child.Reset(child.Release());
    impl->input.Reset(inputWrite.Release());
    impl->outputRead.Reset(outputRead.Release());
    impl->errorRead.Reset(errorRead.Release());
    impl->outputClosed = false;
    impl->started = true;
    impl->outputReader = std::thread([this] { impl->Drain(impl->outputRead.Get(), true); });
    impl->errorReader = std::thread([this] { impl->Drain(impl->errorRead.Get(), false); });
    return true;
#else
    // Everything the child needs is built before fork: only async-signal-safe calls
    // run between fork and exec.
    const std::string path = request.executable.string();
    std::vector<std::string> argumentText{path};
    argumentText.insert(argumentText.end(), request.arguments.begin(), request.arguments.end());
    std::vector<char*> argv;
    for (auto& text : argumentText)
        argv.push_back(text.data());
    argv.push_back(nullptr);
    std::vector<std::string> environmentText;
    for (const auto& [name, value] : request.environment)
        environmentText.push_back(name + "=" + value);
    std::vector<char*> envp;
    for (auto& text : environmentText)
        envp.push_back(text.data());
    envp.push_back(nullptr);
    const std::string directoryText = directory.string();

    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    int errors[2] = {-1, -1};
    int failure[2] = {-1, -1};
    const auto closeAll = [&]()
    {
        for (int* pair : {input, output, errors, failure})
        {
            for (int index = 0; index < 2; ++index)
            {
                if (pair[index] >= 0)
                    close(pair[index]);
                pair[index] = -1;
            }
        }
    };
    // A socket for standard input so a write to a child that has gone away fails with
    // an error instead of raising SIGPIPE across the whole process.
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input) != 0 || pipe2(output, O_CLOEXEC) != 0 || pipe2(errors, O_CLOEXEC) != 0 ||
        pipe2(failure, O_CLOEXEC) != 0)
    {
        closeAll();
        error = "Could not create the server's standard streams.";
        return false;
    }
    const long openMaximum = std::min<long>(sysconf(_SC_OPEN_MAX) > 0 ? sysconf(_SC_OPEN_MAX) : 1024, 65536);
    const pid_t pid = fork();
    if (pid < 0)
    {
        closeAll();
        error = "Could not start the server process.";
        return false;
    }
    if (pid == 0)
    {
        setpgid(0, 0);
        if (dup2(input[1], 0) < 0 || dup2(output[1], 1) < 0 || dup2(errors[1], 2) < 0)
            _exit(127);
        for (int descriptor = 3; descriptor < openMaximum; ++descriptor)
        {
            if (descriptor != failure[1])
                close(descriptor);
        }
        if (chdir(directoryText.c_str()) == 0)
            execve(path.c_str(), argv.data(), envp.data());
        const int code = errno;
        [[maybe_unused]] const auto written = write(failure[1], &code, sizeof(code));
        _exit(127);
    }
    setpgid(pid, pid);
    close(input[1]);
    close(output[1]);
    close(errors[1]);
    close(failure[1]);
    input[1] = output[1] = errors[1] = failure[1] = -1;
    int code = 0;
    ssize_t reported = 0;
    do
        reported = read(failure[0], &code, sizeof(code));
    while (reported < 0 && errno == EINTR);
    close(failure[0]);
    failure[0] = -1;
    if (reported > 0)
    {
        waitpid(pid, nullptr, 0);
        closeAll();
        error = "Could not start the server (error " + std::to_string(code) + ").";
        return false;
    }
    impl->child = pid;
    impl->input = input[0];
    impl->outputRead = output[0];
    impl->errorRead = errors[0];
    impl->outputClosed = false;
    impl->started = true;
    impl->outputReader = std::thread([this] { impl->Drain(impl->outputRead, true); });
    impl->errorReader = std::thread([this] { impl->Drain(impl->errorRead, false); });
    return true;
#endif
}

bool OwnedProcess::Write(const std::string& bytes, std::string& error)
{
    if (!impl->started || impl->stopping.load())
    {
        error = "The server is not running.";
        return false;
    }
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
#ifdef _WIN32
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - sent, 65536));
        if (!WriteFile(impl->input.Get(), bytes.data() + sent, chunk, &written, nullptr) || written == 0)
        {
            error = "The server stopped reading its input.";
            return false;
        }
        sent += written;
#else
        const ssize_t written = send(impl->input, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
        {
            error = "The server stopped reading its input.";
            return false;
        }
        sent += static_cast<std::size_t>(written);
#endif
    }
    return true;
}

std::string OwnedProcess::Read(const std::chrono::milliseconds wait)
{
    std::unique_lock lock(impl->mutex);
    impl->arrived.wait_for(lock, wait, [this] { return !impl->output.empty() || impl->outputClosed; });
    std::string bytes;
    bytes.swap(impl->output);
    return bytes;
}

bool OwnedProcess::Running() const
{
    std::lock_guard lock(impl->mutex);
    return impl->started && !impl->stopping.load() && (!impl->outputClosed || !impl->output.empty());
}

std::string OwnedProcess::ErrorOutput() const
{
    std::lock_guard lock(impl->mutex);
    return utf8::Sanitize(impl->errors);
}

std::string OwnedProcess::StopReason() const
{
    std::lock_guard lock(impl->mutex);
    return impl->stopReason;
}

void OwnedProcess::Terminate()
{
    if (!impl->started)
        return;
    impl->Kill();
    impl->stopping.store(true);
    if (impl->outputReader.joinable())
        impl->outputReader.join();
    if (impl->errorReader.joinable())
        impl->errorReader.join();
#ifdef _WIN32
    if (impl->child.Valid())
        WaitForSingleObject(impl->child.Get(), 5000);
    impl->input.Reset();
    impl->outputRead.Reset();
    impl->errorRead.Reset();
    impl->child.Reset();
    impl->job.Reset();
#else
    if (impl->child > 0)
        waitpid(impl->child, nullptr, 0);
    for (int* descriptor : {&impl->input, &impl->outputRead, &impl->errorRead})
    {
        if (*descriptor >= 0)
            close(*descriptor);
        *descriptor = -1;
    }
    impl->child = -1;
#endif
    {
        std::lock_guard lock(impl->mutex);
        impl->outputClosed = true;
        impl->started = false;
    }
    impl->arrived.notify_all();
}

} // namespace revia::process
