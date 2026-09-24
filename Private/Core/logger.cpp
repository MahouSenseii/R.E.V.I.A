#include "Core/logger.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>
#include "Library/enumLibrary.h"
using namespace std;

namespace
{
    std::mutex LogMutex;
    // Held open between lines, guarded by LogMutex. Opening, appending, and closing the
    // file for every line meant a directory check and a file open per line -- dozens a
    // second during a spoken reply, in a folder a sync client watches. It is reopened
    // only when the resolved path changes, which is what a test that moves its working
    // directory relies on.
    std::ofstream LogFile;
    std::filesystem::path LogFilePath;
    // Bytes in LogFile as far as this process knows. She runs for weeks and logs every
    // turn, every screen check and every voice phrase; without a bound revia.log only
    // ever grew.
    std::uintmax_t LogFileBytes = 0;
    constexpr std::uintmax_t MaximumLogBytes = 32ULL * 1024ULL * 1024ULL;

    std::string Timestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif
        std::ostringstream stream;
        stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
               << '.' << std::setfill('0') << std::setw(3) << milliseconds.count();
        return stream.str();
    }
}

logger::logger() = default;

logger::~logger() = default;

void logger::Log(const std::string &message) {
    Write("Log", message, false);
}

void logger::Warning(const std::string &reason, const std::source_location &location) {
    std::ostringstream message;
    message << "File: " << location.file_name()
            << " Line: " << location.line()
            << " Function: " << location.function_name()
            << " Reason: " << reason;
    Write("Warning", message.str(), true);
}

void logger::Error(const std::string &reason, const std::source_location &location) {
    std::ostringstream message;
    message << "File: " << location.file_name()
            << " Line: " << location.line()
            << " Function: " << location.function_name()
            << " Reason: " << reason;
    Write("Error", message.str(), true);
}

void logger::Timing(const std::string& scope, const std::vector<latencySample>& samples)
{
    if (samples.empty())
    {
        return;
    }

    const latencySample* slowest = nullptr;
    std::ostringstream message;
    message << scope << " | ";
    message << std::fixed << std::setprecision(1);
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const latencySample& sample = samples[index];
        if (index > 0)
        {
            message << " | ";
        }
        message << sample.stage << '=' << sample.milliseconds << "ms";
        if (!sample.bAggregate && (!slowest || sample.milliseconds > slowest->milliseconds))
        {
            slowest = &sample;
        }
    }
    if (slowest)
    {
        message << " | slowest=" << slowest->stage << '(' << slowest->milliseconds << "ms)";
    }
    Write("Timing", message.str(), false);
}

void logger::PromptBreakdown(
    const std::string& scope,
    const std::vector<promptSection>& sections)
{
    if (sections.empty())
    {
        return;
    }

    // The guaranteed stable prefix and potentially changing suffix. An unchanged
    // beginning of the history may also be reused: these are prompt sizes, not a
    // measurement of how many tokens the backend actually evaluated.
    std::size_t total = 0;
    std::size_t reusablePrefix = 0;
    bool prefixIntact = true;
    const promptSection* largestVolatile = nullptr;
    for (const promptSection& section : sections)
    {
        total += section.characters;
        if (prefixIntact && section.stable)
        {
            reusablePrefix += section.characters;
        }
        else
        {
            prefixIntact = false;
            if (!largestVolatile || section.characters > largestVolatile->characters)
            {
                largestVolatile = &section;
            }
        }
    }

    std::ostringstream message;
    message << scope;
    for (const promptSection& section : sections)
    {
        message << " | " << section.name << '=' << section.characters << 'c';
        if (!section.stable)
        {
            message << '*';
        }
    }
    message << " | total=" << total << 'c';
    message << " | stable_prefix=" << reusablePrefix << 'c';
    message << " | variable_suffix=" << (total - reusablePrefix) << 'c';
    if (largestVolatile)
    {
        message << " | largest_volatile=" << largestVolatile->name
                << '(' << largestVolatile->characters << "c)";
    }
    message << " | *=variable; unchanged history prefix may also be cached";
    Write("Prompt", message.str(), false);
}

void logger::Write(
    const std::string& severity,
    const std::string& message,
    const bool bUseErrorStream)
{
    const std::string line = '[' + Timestamp() + "] [" + severity + "] " + message;
    {
        std::lock_guard lock(LogMutex);
        std::ostream& output = bUseErrorStream ? std::cerr : std::cout;
        output << line << std::endl;

        std::error_code error;
        std::filesystem::path wanted =
            std::filesystem::absolute(std::filesystem::path(ReviaLogDirectory()), error) /
            "revia.log";
        if (error) wanted = std::filesystem::path("Logs") / "revia.log";
        if (!LogFile.is_open() || wanted != LogFilePath)
        {
            if (LogFile.is_open()) LogFile.close();
            LogFile.clear();
            std::filesystem::create_directories(wanted.parent_path(), error);
            LogFile.open(wanted, std::ios::app);
            LogFilePath = wanted;
            LogFileBytes = std::filesystem::file_size(wanted, error);
            if (error) LogFileBytes = 0;
        }
        if (LogFile && LogFileBytes >= MaximumLogBytes)
        {
            // Closed first: Windows will not rename a file this process holds open.
            LogFile.close();
            LogFile.clear();
            (void)RotateLogFile(wanted, MaximumLogBytes);
            LogFile.open(wanted, std::ios::app);
            // Zero either way. After a rotation the file is new; after a refused one --
            // another program holding it -- the next attempt waits for another full
            // allowance instead of reopening the file on every line.
            LogFileBytes = 0;
        }
        if (LogFile)
        {
            // Flushed per line: a crash must not lose the lines that explain it.
            LogFile << line << '\n' << std::flush;
            LogFileBytes += line.size() + 1;
        }
        else
        {
            // Try again on the next line rather than going silent for the session.
            LogFile.close();
            LogFile.clear();
            LogFilePath.clear();
        }
    }

    std::function<void(const std::string&)> activeSink;
    {
        std::lock_guard lock(sinkMutex);
        activeSink = sink;
    }
    if (activeSink)
    {
        activeSink(line);
    }
}

std::string ReviaLogDirectory()
{
    if (const char* overridden = std::getenv("REVIA_LOG_DIR");
        overridden != nullptr && *overridden != '\0')
    {
        return overridden;
    }
    return "Logs";
}

bool RotateLogFile(const std::filesystem::path& path, const std::uintmax_t maximumBytes)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size < maximumBytes) return false;
    std::filesystem::path backup = path;
    backup += ".1";
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::rename(path, backup, error);
    return !error;
}

void logger::SetSink(std::function<void(const std::string&)> newSink)
{
    std::lock_guard lock(sinkMutex);
    sink = std::move(newSink);
}

bool logger::Check(bool bCondition, logSeverity severity, const std::string &reason, const std::source_location &location) {
    if (!bCondition)
    {
        if (severity == logSeverity::Error)
        {
            Error(reason, location);
        }
        else
        {
           Warning(reason, location);

        }
        return false;
    }
    return true;
}
