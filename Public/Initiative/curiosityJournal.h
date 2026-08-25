#pragma once

#include <chrono>
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
    bool Append(const CuriosityRecord& record, std::string& outError);
    [[nodiscard]] std::vector<CuriosityRecord> Recent(std::size_t maximum) const;

    [[nodiscard]] static std::string NormalizeTopic(const std::string& topic);

private:
    mutable std::mutex mutex;
    std::filesystem::path journalPath;
    std::vector<CuriosityRecord> records;
};

} // namespace revia::initiative
