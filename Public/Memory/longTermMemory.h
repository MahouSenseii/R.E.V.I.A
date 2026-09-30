#pragma once

// Forward declared so the sqlite3 header stays out of every translation unit that
// merely wants to read a memory.
struct sqlite3;

#include "Memory/memoryTypes.h"
#include "Memory/temporalQuery.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>


struct EmbeddingBackfillPage
{
    std::vector<memoryEntry> entries;
    std::int64_t nextRowId = 0;
    bool hasMore = false;
    std::string error;
};

class longTermMemory
{
public:
    explicit longTermMemory(std::string path = "Memory/revia_memory.db");
    ~longTermMemory();

    // Process-wide because prompt construction and the background memory agent own
    // separate store objects. Applied to every newly opened SQLite connection.
    static void ConfigureCache(int cacheMiB, int mmapMiB);

    std::vector<memoryEntry> Load() const;
    // Returns the new or deduplicated row id when requested, so optional work
    // can address the accepted memory without saving its content a second time.
    // Duplicates retain their stored summary; an incoming vector is accepted only
    // for identical text. Deferred embedding work must use the stored summary.
    bool Save(const memoryDecision& decision, bool& outWasAdded, std::string* outMemoryId = nullptr) const;
    bool HasMemories() const;
    // Time references add a created_at-index candidate list without filtering out other rankings.
    // nowEpoch selects the resolution clock; zero uses the system clock.
    std::vector<memoryEntry> Search(const std::string& query, std::size_t maxEntries = 6, const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "", std::int64_t nowEpoch = 0) const;
    std::string BuildPromptBlock(const std::string& query = "", std::size_t maxEntries = 6, const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "", std::int64_t nowEpoch = 0) const;
    std::vector<memoryEntry> LoadMissingEmbeddings(const std::string& embeddingModel, std::size_t maxEntries = 25) const;
    // Row-id traversal moves past failed rows without an offset into a shrinking
    // result set. Restarting a scan at zero rediscovers failures and newly added rows.
    EmbeddingBackfillPage ScanMissingEmbeddings(const std::string& embeddingModel,
        std::int64_t afterRowId, std::size_t maxEntries = 25) const;
    bool NeedsEmbedding(const std::string& memoryId, const std::string& embeddingModel) const;
    bool SaveEmbedding(const std::string& memoryId, const std::string& embeddingModel, const std::vector<float>& embedding) const;

private:
    struct Connection;
    // Opens one connection lazily and retains it for the store lifetime.
    [[nodiscard]] sqlite3* Acquire() const;
    EmbeddingBackfillPage ReadMissingEmbeddings(const std::string& embeddingModel,
        std::size_t maxEntries, std::optional<std::int64_t> afterRowId) const;

    std::string memoryPath;
    mutable std::mutex connectionMutex;
    mutable std::shared_ptr<Connection> connection;
};
