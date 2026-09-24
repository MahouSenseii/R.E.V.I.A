#include "Core/crashDiagnostics.h"

#include "Core/exitReporter.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace revia::core
{

namespace
{

std::filesystem::path directory;
std::mutex writeMutex;
std::atomic<bool> installed = false;

std::string Timestamp()
{
    const std::time_t time = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

std::string FileStamp()
{
    std::string stamp = Timestamp();
    for (char& character : stamp)
    {
        if (character == ':' || character == ' ')
        {
            character = '-';
        }
    }
    return stamp;
}

#ifdef _WIN32
std::string DescribeExceptionCode(const DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_STACK_OVERFLOW:
            // Named explicitly because it is the one that normally leaves no trace at all,
            // and because its usual cause is unbounded recursion rather than a bad pointer.
            return "stack overflow (runaway recursion)";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
        case 0xE06D7363: return "unhandled C++ exception";
        default: return "exception";
    }
}

void WriteMinidump(EXCEPTION_POINTERS* pointers)
{
    if (directory.empty())
    {
        return;
    }
    const std::filesystem::path dumpPath = directory / ("crash-" + FileStamp() + ".dmp");
    const HANDLE file = CreateFileW(
        dumpPath.wstring().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = pointers;
    information.ClientPointers = FALSE;
    MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        MiniDumpWithIndirectlyReferencedMemory,
        pointers == nullptr ? nullptr : &information,
        nullptr,
        nullptr);
    CloseHandle(file);
    CrashDiagnostics::Record("Crash", "A minidump was written to " + dumpPath.string());
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers)
{
    const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr
        ? pointers->ExceptionRecord->ExceptionCode
        : 0;
    std::ostringstream stream;
    stream << DescribeExceptionCode(code) << " (0x" << std::hex << code << std::dec << ')';
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
    {
        stream << " at 0x" << std::hex
            << reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress)
            << std::dec;
    }
    CrashDiagnostics::Record("Crash", stream.str());
    ExitReporter::Record(ExitReason::Crash, stream.str());
    WriteMinidump(pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void OnTerminate()
{
    std::string detail = "std::terminate was called";
    if (const std::exception_ptr active = std::current_exception())
    {
        try
        {
            std::rethrow_exception(active);
        }
        catch (const std::exception& error)
        {
            detail = std::string("unhandled exception: ") + error.what();
        }
        catch (...)
        {
            detail = "unhandled exception of an unknown type";
        }
    }
    CrashDiagnostics::Record("Crash", detail);
    ExitReporter::Record(ExitReason::Crash, detail);
#ifdef _WIN32
    WriteMinidump(nullptr);
#endif
    std::_Exit(3);
}

void OnSignal(const int number)
{
    const std::string detail = "fatal signal " + std::to_string(number);
    CrashDiagnostics::Record("Crash", detail);
    ExitReporter::Record(ExitReason::Crash, detail);
    std::_Exit(3);
}

} // namespace

std::filesystem::path CrashDiagnostics::LogPath()
{
    return directory.empty() ? std::filesystem::path("crash.log") : directory / "crash.log";
}

void CrashDiagnostics::Record(const std::string& category, const std::string& message)
{
    std::lock_guard lock(writeMutex);
    std::ofstream file(LogPath(), std::ios::app);
    if (!file.is_open())
    {
        return;
    }
    file << '[' << Timestamp() << "] [" << category << "] " << message << '\n';
    file.flush();
}

void CrashDiagnostics::InstallPlatformHandlers()
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnUnhandledException);
    // Without a reserved margin there is no stack left to run a handler on, which is
    // exactly why a stack overflow normally dies silently. This buys the handler enough
    // room to write the log line that names it.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
#endif
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnSignal);
    std::signal(SIGSEGV, OnSignal);
    std::signal(SIGILL, OnSignal);
    std::signal(SIGFPE, OnSignal);
}

void CrashDiagnostics::Install(std::filesystem::path logDirectory)
{
    if (installed.exchange(true))
    {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(logDirectory, error);
    directory = std::move(logDirectory);
    InstallPlatformHandlers();
}

} // namespace revia::core
