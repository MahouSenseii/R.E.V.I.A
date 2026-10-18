#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::workspace
{
enum class GuestOperation { ListDirectory, ReadText, WriteText, ExecuteProcess };

struct GuestIdentity
{
    std::string environmentId;
    std::string bootId;
};

// Paths are relative POSIX workspace paths, never host filesystem paths.
struct GuestRequest
{
    GuestOperation operation = GuestOperation::ListDirectory;
    std::string path;
    std::string content;
    std::string expectedDigest;
    std::string executable;
    std::vector<std::string> arguments;
    std::string workingDirectory;
    int timeoutMs = 30000;
    // Captured by host task admission, never decoded from a model proposal.
    GuestIdentity expectedIdentity;
};

struct GuestLimits
{
    std::size_t maxReadBytes = 65536;
    std::size_t maxWriteBytes = 65536;
    std::size_t maxOutputBytes = 65536;
    std::size_t maxEntries = 256;
    int maxTimeoutMs = 120000;
    std::vector<std::string> approvedExecutables;
};

struct GuestResult
{
    bool attempted = false;
    bool succeeded = false;
    bool uncertainEffect = false;
    // A verified negative receipt permits diagnosis, but never automatic replay.
    bool receiptVerified = false;
    std::string code;
    std::string message;
    std::string content;
    std::string digest;
    std::vector<std::string> entries;
    std::string standardOutput;
    std::string standardError;
    std::int64_t exitCode = -1;
    bool timedOut = false;
    bool cancelled = false;
    bool outputTruncated = false;
    GuestIdentity identity;
};
}
