#include "Process/processExecutor.h"

#include "Core/utf8.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::process
{
namespace
{

#ifdef _WIN32
class Handle
{
  public:
    explicit Handle(HANDLE value = nullptr) : value(value)
    {
    }
    ~Handle()
    {
        Reset();
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const
    {
        return value;
    }
    [[nodiscard]] bool Valid() const
    {
        return value && value != INVALID_HANDLE_VALUE;
    }
    void Reset(HANDLE next = nullptr)
    {
        if (Valid())
            CloseHandle(value);
        value = next;
    }

  private:
    HANDLE value;
};

std::wstring Wide(const std::string& value)
{
    return actions::Utf8ToPath(value).native();
}

std::wstring Quote(const std::wstring& value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

struct EnvironmentOrder
{
    bool operator()(const std::wstring& left, const std::wstring& right) const
    {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

std::vector<wchar_t> Environment(const ProcessRequest& request)
{
    // Start from the minimum Windows runtime environment, never the parent's secrets.
    std::map<std::wstring, std::wstring, EnvironmentOrder> values;
    std::array<wchar_t, MAX_PATH + 1> windows{};
    const auto count = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (count > 0 && count < windows.size())
    {
        values[L"SystemRoot"] = windows.data();
        values[L"WINDIR"] = windows.data();
    }
    for (const auto& [name, value] : request.environment)
        values[Wide(name)] = Wide(value);
    std::vector<wchar_t> block;
    for (const auto& [name, value] : values)
    {
        const auto entry = name + L"=" + value;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1)
        block.push_back(L'\0');
    return block;
}

bool Pipe(Handle& reader, Handle& writer)
{
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &security, 64U * 1024U))
        return false;
    reader.Reset(read);
    writer.Reset(write);
    return SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) != FALSE;
}

bool Drain(HANDLE pipe, std::string& destination, std::size_t& remaining, bool& truncated)
{
    std::array<char, 4096> bytes{};
    bool consumed = false;
    // Bound work per poll even when the child continuously fills its pipe.
    for (int pass = 0; pass < 16; ++pass)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
            return consumed;
        DWORD read = 0;
        if (!ReadFile(pipe, bytes.data(), std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &read, nullptr) || read == 0)
            return consumed;
        consumed = true;
        const auto keep = std::min<std::size_t>(remaining, read);
        destination.append(bytes.data(), keep);
        remaining -= keep;
        truncated = truncated || keep < read;
    }
    return consumed;
}
#endif

} // namespace

bool ProcessExecutor::Handles(const actions::ActionType type) const
{
    return type == actions::ActionType::ExecuteProcess;
}

actions::ActionResult ProcessExecutor::Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision)
{
    actions::ActionResult result;
    result.dryRun = request.dryRun;
    result.backend = "native_process";
    if (!Handles(request.type) || !request.beforeEffect || decision.canonicalExecutable.empty() || decision.canonicalSource.empty() ||
        decision.processOutputLimitBytes == 0)
    {
        result.message = "Native process execution requires a captured, bounded runtime admission.";
        return result;
    }
    const auto admission = [&] { return request.beforeEffect(actions::PathToUtf8(decision.canonicalExecutable)); };
    if (const auto refusal = admission(); !refusal.empty())
    {
        result.message = refusal;
        return result;
    }
    if (request.dryRun)
    {
        result.succeeded = true;
        result.message = "Dry run: the admitted process would execute; no child was started.";
        return result;
    }
#ifndef _WIN32
    result.message = "Native process execution is unavailable on this platform.";
    return result;
#else
    std::wstring command = Quote(decision.canonicalExecutable.native());
    for (const auto& argument : request.process.arguments)
        command += L" " + Quote(Wide(argument));
    if (command.size() >= 32767)
    {
        result.message = "The native process argument vector exceeds Windows' command-line limit.";
        return result;
    }
    auto environment = Environment(request.process);
    Handle outputRead, outputWrite, errorRead, errorWrite;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.Valid() || !input.Valid() || !SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !Pipe(outputRead, outputWrite) || !Pipe(errorRead, errorWrite))
    {
        result.message = "Could not establish the native child lifetime or output handles.";
        return result;
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> storage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
    {
        result.message = "Could not initialize the child's explicit handle list.";
        return result;
    }
    const std::array<HANDLE, 3> inherited{input.Get(), outputWrite.Get(), errorWrite.Get()};
    const bool handlesSet = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.Get();
    startup.StartupInfo.hStdOutput = outputWrite.Get();
    startup.StartupInfo.hStdError = errorWrite.Get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information{};
    const auto refusal = admission();
    const bool created = handlesSet && refusal.empty() &&
                         CreateProcessW(decision.canonicalExecutable.c_str(), command.data(), nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                             environment.data(), decision.canonicalSource.c_str(), &startup.StartupInfo, &information);
    const DWORD creationError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created)
    {
        result.message =
            refusal.empty() ? "Could not start the admitted process (Windows error " + std::to_string(creationError) + ")." : refusal;
        return result;
    }
    Handle child(information.hProcess), thread(information.hThread);
    outputWrite.Reset();
    errorWrite.Reset();
    if (!AssignProcessToJobObject(job.Get(), child.Get()))
    {
        TerminateProcess(child.Get(), 1);
        WaitForSingleObject(child.Get(), INFINITE);
        result.message = "Process tree ownership could not be established; the suspended child was terminated.";
        return result;
    }
    result.process.emplace();
    auto& receipt = *result.process;
    receipt.authorityStamp = request.authorityStamp;
    const auto started = std::chrono::steady_clock::now();
    const auto startRefusal = admission();
    if (!startRefusal.empty() || ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
    {
        TerminateJobObject(job.Get(), 1);
        WaitForSingleObject(child.Get(), INFINITE);
        receipt.cancelled = !startRefusal.empty();
        result.message = startRefusal.empty() ? "Could not resume the owned process." : startRefusal;
        return result;
    }
    result.attempted = true;
    std::size_t remaining = std::min<std::size_t>(decision.processOutputLimitBytes, 1024U * 1024U);
    for (;;)
    {
        const bool outputArrived = Drain(outputRead.Get(), receipt.standardOutput, remaining, receipt.outputTruncated);
        const bool errorArrived = Drain(errorRead.Get(), receipt.standardError, remaining, receipt.outputTruncated);
        const DWORD wait = WaitForSingleObject(child.Get(), 0);
        if (wait == WAIT_OBJECT_0)
            break;
        const auto currentRefusal = admission();
        receipt.cancelled = !currentRefusal.empty();
        receipt.timedOut = std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(request.process.timeoutMs);
        if (receipt.cancelled || receipt.timedOut || wait == WAIT_FAILED)
        {
            result.message = receipt.cancelled  ? currentRefusal
                             : receipt.timedOut ? "The process exceeded its timeout."
                                                : "The child wait failed.";
            TerminateJobObject(job.Get(), 1);
            WaitForSingleObject(child.Get(), INFINITE);
            break;
        }
        if (outputArrived || errorArrived)
            SwitchToThread();
        else
            WaitForSingleObject(child.Get(), 10);
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(child.Get(), &exitCode))
        receipt.exitCode = exitCode;
    // The parent exiting does not authorize a detached child. Kill remaining descendants
    // before draining so an inherited pipe cannot keep this action alive indefinitely.
    TerminateJobObject(job.Get(), 1);
    job.Reset();
    Drain(outputRead.Get(), receipt.standardOutput, remaining, receipt.outputTruncated);
    Drain(errorRead.Get(), receipt.standardError, remaining, receipt.outputTruncated);
    receipt.standardOutput = utf8::Sanitize(receipt.standardOutput);
    receipt.standardError = utf8::Sanitize(receipt.standardError);
    if (receipt.standardOutput.size() + receipt.standardError.size() > decision.processOutputLimitBytes)
    {
        receipt.outputTruncated = true;
        utf8::Truncate(receipt.standardOutput, decision.processOutputLimitBytes);
        utf8::Truncate(receipt.standardError, decision.processOutputLimitBytes - receipt.standardOutput.size());
    }
    result.content = receipt.standardOutput;
    result.succeeded = receipt.exitCode == 0 && !receipt.cancelled && !receipt.timedOut && result.message.empty();
    if (result.message.empty())
        result.message = "Process exited with code " + std::to_string(receipt.exitCode) + ".";
    if (receipt.outputTruncated)
        result.message += " Captured output was truncated at the configured limit.";
    return result;
#endif
}

} // namespace revia::process
