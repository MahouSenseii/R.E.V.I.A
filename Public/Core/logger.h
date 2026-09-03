#pragma once


#include <string>
#include <source_location>
#include <functional>
#include <mutex>
#include <vector>

#include "Library/structLibrary.h"

enum class logSeverity;

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
