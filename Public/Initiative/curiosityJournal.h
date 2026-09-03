#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace revia::initiative
{

struct CuriosityRecord
{
    std::string topic;
    std::string query;
    std::vector<std::string> sources;
    std::string outcome;
    std::chrono::system_clock::time_point occurredAt =
        std::chrono::system_clock::now();
};

// A deliberately small durable index of autonomous topics. It stores no raw page body
// and no private reasoning; its job is to prevent Revia from rediscovering and repeating
// the same thought after every restart.
class CuriosityJournal
{
public:
    bool Initialize(const std::filesystem::path& path, std::string& outError);
    [[nodiscard]] bool WasRecentlyConsidered(
        const std::string& topic,
        std::chrono::minutes window,
        std::chrono::system_clock::time_point now) const;
    // A separate global pace for network work. Topic deduplication prevents repetition,
    // while this prevents a stream of different model-selected topics from turning the
    // background learner into a crawler.
    [[nodiscard]] bool WasResearchRecentlyAttempted(
        std::chrono::seconds window,
        std::chrono::system_clock::time_point now) const;
    bool Append(const CuriosityRecord& record, std::string& outError);
    [[nodiscard]] std::vector<CuriosityRecord> Recent(std::size_t maximum) const;

    [[nodiscard]] static std::string NormalizeTopic(const std::string& topic);

private:
    // Rewrites the journal to just the retained records. Caller holds the mutex.
    [[nodiscard]] bool CompactUnlocked(std::string& outError);

    mutable std::mutex mutex;
    std::size_t appendsSinceCompaction = 0;
    // Bytes written to the journal, tracked in process. Directory-entry file sizes are
    // stale for a file being appended to on Windows, so this is the only reliable
    // trigger for compaction.
    std::uintmax_t journalBytes = 0;
    std::filesystem::path journalPath;
    std::vector<CuriosityRecord> records;
};

} // namespace revia::initiative
