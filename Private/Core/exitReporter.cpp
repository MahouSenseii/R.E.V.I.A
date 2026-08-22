#include "Core/exitReporter.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace revia::core
{

namespace
{

std::filesystem::path directory;
std::string previousUncleanExit;
std::mutex ledgerMutex;
std::atomic<bool> recorded = false;
std::atomic<bool> opened = false;

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

unsigned long CurrentProcessId()
{
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

void Append(const std::string& line)
{
    std::ofstream file(ExitReporter::LedgerPath(), std::ios::app);
    if (!file.is_open())
    {
        return;
    }
    file << '[' << Timestamp() << "] " << line << '\n';
    file.flush();
}

} // namespace

std::string ToString(const ExitReason reason)
{
    switch (reason)
    {
        case ExitReason::UserCommand: return "user command";
        case ExitReason::TrayQuit: return "quit from the tray";
        case ExitReason::WindowClosed: return "window closed";
        case ExitReason::SmokeTest: return "smoke test";
        case ExitReason::StartupFailure: return "startup failure";
        case ExitReason::Crash: return "crash";
        case ExitReason::EventLoopEnded: return "event loop ended";
        case ExitReason::SystemShutdown: return "Windows ended the session";
        case ExitReason::ExternalTermination: return "terminated without recording a reason";
    }
    return "unknown";
}

std::filesystem::path ExitReporter::LedgerPath()
{
    return directory.empty()
        ? std::filesystem::path("session-exits.log")
        : directory / "session-exits.log";
}

std::filesystem::path ExitReporter::MarkerPath()
{
    return directory.empty()
        ? std::filesystem::path("session.open")
        : directory / "session.open";
}

bool ExitReporter::HasRecorded()
{
    return recorded.load();
}

std::string ExitReporter::PreviousUncleanExit()
{
    std::lock_guard lock(ledgerMutex);
    return previousUncleanExit;
}

std::string ExitReporter::Begin(std::filesystem::path logDirectory)
{
    if (opened.exchange(true))
    {
        return {};
    }
    std::error_code error;
    std::filesystem::create_directories(logDirectory, error);
    directory = std::move(logDirectory);

    std::lock_guard lock(ledgerMutex);

    // A marker left behind is the whole point: the previous run could not tell us why it
    // ended, and its silence is now on the record instead of being lost.
    std::string previous;
    {
        std::ifstream marker(MarkerPath());
        if (marker.is_open())
        {
            std::string startedAt;
            std::string processId;
            std::getline(marker, startedAt);
            std::getline(marker, processId);
            std::ostringstream stream;
            stream << "The previous session (started " << startedAt << ", process "
                << processId << ") ended without recording a reason. That means it was "
                   "terminated from outside, lost power, or failed too hard to report. "
                   "Check crash.log for the same timestamp before assuming it was closed "
                   "deliberately.";
            previous = stream.str();
        }
    }
    if (!previous.empty())
    {
        Append("SESSION ENDED  " + ToString(ExitReason::ExternalTermination) +
            " -- " + previous);
    }
    previousUncleanExit = previous;

    {
        std::ofstream marker(MarkerPath(), std::ios::trunc);
        if (marker.is_open())
        {
            marker << Timestamp() << '\n' << CurrentProcessId() << '\n';
            marker.flush();
        }
    }
    Append("SESSION STARTED  process " + std::to_string(CurrentProcessId()));
    return previous;
}

void ExitReporter::Record(const ExitReason reason, const std::string& detail)
{
    // The first reason wins. A crash records itself and then the event loop unwinds and
    // reports that it ended, which is true and would bury the cause.
    if (recorded.exchange(true))
    {
        return;
    }
    std::lock_guard lock(ledgerMutex);
    std::string line = "SESSION ENDED  " + ToString(reason);
    if (!detail.empty())
    {
        line += " -- " + detail;
    }
    Append(line);

    // Removed only now, so the marker's absence means "a reason was recorded" and its
    // presence at the next start means nothing was.
    std::error_code error;
    std::filesystem::remove(MarkerPath(), error);
}

} // namespace revia::core
