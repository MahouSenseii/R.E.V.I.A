#pragma once

#include "Runtime/runtimeStamp.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace revia::process
{

struct ProcessSettings
{
    bool enabled = false;
    bool allowTaskExecution = false;
    std::vector<std::filesystem::path> approvedExecutables;
    bool allowCommandInterpreters = false;
    int maxTimeoutMs = 30000;
    std::size_t maxOutputBytes = 65536;
    std::size_t maxEnvironmentBytes = 16384;
    std::vector<std::string> approvedEnvironmentNames;
};

struct ProcessRequest
{
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> environment;
    int timeoutMs = 30000;
};

struct ProcessResult
{
    std::string standardOutput;
    std::string standardError;
    std::int64_t exitCode = -1;
    bool timedOut = false;
    bool cancelled = false;
    bool outputTruncated = false;
    runtime::RuntimeStamp authorityStamp;
};

} // namespace revia::process
