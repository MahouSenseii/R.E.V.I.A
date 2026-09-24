#pragma once


#include <cstdint>
#include <filesystem>
#include <string>
#include <source_location>
#include <functional>
#include <mutex>
#include <vector>

#include "Library/structLibrary.h"

enum class logSeverity;

// Where Revia's own logs go: REVIA_LOG_DIR when set, otherwise "Logs" under the working
// directory. The override exists for test runs, which start from the build output and
// otherwise append fixture goals and smoke-test sessions to the owner's real logs.
[[nodiscard]] std::string ReviaLogDirectory();

// Moves `path` to `path.1`, replacing an older one, once it has reached maximumBytes, so
// a log that is written for months keeps one file's worth of history rather than all of
// it. True when it was moved; false when it is still small or the move was refused.
bool RotateLogFile(const std::filesystem::path& path, std::uintmax_t maximumBytes);

class logger
{
public:
    logger();
    ~logger();


    void Log(const std::string& message);
    void Warning(const std::string& reason,const std::source_location& location = std::source_location::current());
    void Error(const std::string& reason,const std::source_location& location = std::source_location::current());
    void Timing(const std::string& scope, const std::vector<latencySample>& samples);
    // What the prompt was made of, in prompt order, and how much of it could not be
    // reused from the previous turn. Separate from Timing because a size is not a
    // duration and averaging the two into one line makes neither readable.
    void PromptBreakdown(
        const std::string& scope,
        const std::vector<promptSection>& sections);
    void SetSink(std::function<void(const std::string&)> sink);
    bool Check(bool bCondition,  logSeverity severity, const std::string& reason,const std::source_location& location = std::source_location::current());

private:
    void Write(const std::string& severity, const std::string& message, bool bUseErrorStream);
    std::mutex sinkMutex;
    std::function<void(const std::string&)> sink;
};
