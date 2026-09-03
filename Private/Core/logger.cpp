#include "Core/logger.h"
#include <chrono>
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

    // Where the reusable prefix ends. Everything from the first unstable section onward
    // has to be re-evaluated on every turn no matter how much of it repeats, because a
    // prefix cache matches from the start and stops at the first byte that differs.
    // Reporting the total without this number makes a 3000-token prompt look like a
    // caching problem when the real question is how many of those tokens move.
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
    message << " | reusable_prefix=" << reusablePrefix << 'c';
    message << " | reevaluated_every_turn=" << (total - reusablePrefix) << 'c';
    if (largestVolatile)
    {
        message << " | largest_volatile=" << largestVolatile->name
                << '(' << largestVolatile->characters << "c)";
    }
    message << " | *=changes between turns";
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
        std::filesystem::create_directories("Logs", error);
        std::ofstream file("Logs/revia.log", std::ios::app);
        if (file)
        {
            file << line << '\n';
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
