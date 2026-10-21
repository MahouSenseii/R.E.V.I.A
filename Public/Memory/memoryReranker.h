#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::memory
{

struct RerankerSettings
{
    bool enabled = false;
    std::filesystem::path pythonExecutable;
    std::filesystem::path workerScript;
    std::filesystem::path modelDirectory;
    int cpuThreads = 4;
    int timeoutMs = 1000;
    int startupTimeoutMs = 30000;
};

enum class RerankStatus
{
    Succeeded,
    Unavailable,
    Busy,
    InvalidInput,
    InvalidResponse,
    TimedOut,
    Cancelled
};

struct RerankScore
{
    std::size_t index = 0;
    double score = 0.0;
};

struct RerankResult
{
    bool succeeded = false;
    RerankStatus status = RerankStatus::Unavailable;
    std::vector<RerankScore> scores;
    double elapsedMilliseconds = 0.0;
};

// Returns the original order on any failed/incomplete/invalid result. Ties retain
// the caller's fused order. Only positions in the admitted shortlist can be returned.
[[nodiscard]] std::vector<std::size_t> RerankOrder(const RerankResult& result, std::size_t documentCount);

// ReviaSession owns this optional worker. Start loads an already installed local
// CPU model; Rerank never starts processes, downloads models or accepts an endpoint.
class MemoryReranker
{
public:
    MemoryReranker();
    ~MemoryReranker();
    MemoryReranker(const MemoryReranker&) = delete;
    MemoryReranker& operator=(const MemoryReranker&) = delete;

    [[nodiscard]] bool Start(const RerankerSettings& settings, std::stop_token stopToken = {});
    void Stop();
    [[nodiscard]] bool IsAvailable() const;
    [[nodiscard]] std::string LastError() const;
    [[nodiscard]] RerankResult Rerank(const std::string& query, const std::vector<std::string>& documents,
                                    std::stop_token stopToken = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
