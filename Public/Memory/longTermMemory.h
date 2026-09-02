#pragma once

// Forward declared so the sqlite3 header stays out of every translation unit that
// merely wants to read a memory.
struct sqlite3;

#include "Library/structLibrary.h"
#include "Memory/temporalQuery.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>


class longTermMemory
{
public:
    explicit longTermMemory(std::string path = "Memory/revia_memory.db");
    ~longTermMemory();

    // Process-wide because prompt construction and the background memory agent own
    // separate store objects. Applied to every newly opened SQLite connection.
    static void ConfigureCache(int cacheMiB, int mmapMiB);

    std::vector<memoryEntry> Load() const;
    bool Save(const memoryDecision& decision, bool& outWasAdded) const;
    bool HasMemories() const;
    // A query that names a time -- "what did I say yesterday", "that thing from last
    // Tuesday" -- is resolved to a window and answered from the created_at index rather
    // than by ranking every memory and hoping the right one floats up. The window is one
    // more ranked candidate list, not a filter, so a misread phrase costs nothing beyond
    // the ranking that would have happened anyway.
    //
    // nowEpoch is the clock the phrase resolves against; 0 means the system clock, and
    // an explicit value makes the resolution testable.
    std::vector<memoryEntry> Search(
        const std::string& query,
        std::size_t maxEntries = 6,
        const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "",
        std::int64_t nowEpoch = 0) const;
    std::string BuildPromptBlock(
        const std::string& query = "",
        std::size_t maxEntries = 6,
        const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "",
        std::int64_t nowEpoch = 0) const;
    std::vector<memoryEntry> LoadMissingEmbeddings(
        const std::string& embeddingModel,
        std::size_t maxEntries = 25) const;
    bool SaveEmbedding(
        const std::string& memoryId,
        const std::string& embeddingModel,
        const std::vector<float>& embedding) const;

private:
    struct Connection;
    // Opens on first use and stays open for the life of the store.
    //
    // Every call used to open its own connection, and an open is not cheap here: it
    // re-runs the whole schema DDL and the legacy-JSONL import check before the query
    // it was asked for. Measured on 200 rows, a trivial HasMemories() cost 2.6ms of
    // which almost none was the query, and Search -- which runs on every conversation
    // turn -- spent two thirds of its time getting ready to look.
    [[nodiscard]] sqlite3* Acquire() const;

    std::string memoryPath;
    mutable std::mutex connectionMutex;
    mutable std::shared_ptr<Connection> connection;
};
