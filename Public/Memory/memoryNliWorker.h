#pragma once

#include "Memory/semanticDuplicate.h"
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace revia::memory
{
inline constexpr const char* MemoryNliModelIdentity =
    "cross-encoder/nli-deberta-v3-small@fa2804872c3b4bd748f38c0185cc85775361e735";

struct MemoryNliSettings
{
    bool enabled = false;
    std::filesystem::path pythonExecutable;
    std::filesystem::path workerScript;
    std::filesystem::path modelDirectory;
    int cpuThreads = 2;
    int timeoutMs = 2000;
    int startupTimeoutMs = 30000;
};

// Session-owned optional CPU scorer. Only MemoryAgent's admitted background work
// calls Compare; it returns evidence, never a decision to mutate a memory.
class MemoryNliWorker final : public SemanticDuplicateScorer
{
public:
    MemoryNliWorker();
    ~MemoryNliWorker() override;
    MemoryNliWorker(const MemoryNliWorker&) = delete;
    MemoryNliWorker& operator=(const MemoryNliWorker&) = delete;
    [[nodiscard]] bool Start(const MemoryNliSettings&, std::stop_token stopToken = {});
    void Stop();
    [[nodiscard]] bool IsAvailable() const;
    [[nodiscard]] std::string LastError() const;
    [[nodiscard]] NliPairResult Compare(const std::string& left, const std::string& right,
                                      std::stop_token stopToken = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
