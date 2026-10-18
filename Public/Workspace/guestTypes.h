#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::workspace
{
// RunPython, SearchText and ImportFiles serve the chat code tools: a network-less interpreter,
// a search of a shared project, and the explicit copy of a host folder into the guest.
enum class GuestOperation { ListDirectory, ReadText, WriteText, ExecuteProcess, RunPython, SearchText, ImportFiles };

struct GuestIdentity
{
    std::string environmentId;
    std::string bootId;
};

// One text file of a shared project, as the host read it.
struct GuestFile
{
    std::string path;
    std::string content;
};

// A file a sandboxed run left in its out/ folder, decoded and checked against its SHA-256.
struct GuestArtifact
{
    std::string name;
    std::string sha256;
    std::string bytes;
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
    // RunPython: the fresh scratch folder the host named for this run, and the program.
    std::string runId;
    std::string code;
    // SearchText: what to find under `path`.
    std::string pattern;
    bool fixedStrings = false;
    bool ignoreCase = false;
    int maxMatches = 100;
    // ImportFiles: `path` is the staging folder; the last batch publishes it as this project.
    std::vector<GuestFile> files;
    std::string publish;
    // Shown to the person approving, such as a patch's diff. Never sent to the guest.
    std::string review;
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
    // RunPython only. The guest reports the isolation; the host cannot observe it directly.
    bool networkIsolated = false;
    std::vector<GuestArtifact> artifacts;
    // "name: reason" for each file the guest did not copy back.
    std::vector<std::string> artifactsOmitted;
    GuestIdentity identity;
};
}
