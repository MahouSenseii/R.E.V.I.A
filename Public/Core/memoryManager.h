#pragma once

#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"

#include <string>

class memoryManager
{
public:
    memoryManager();
    // Same store, opened at `databasePath` instead of the process-relative default.
    // Exists so callers that need an isolated database -- tests, chiefly -- do not have
    // to reach past this class into longTermMemory directly.
    explicit memoryManager(std::string databasePath);
    ~memoryManager();

    bool SaveAutomaticMemory(const memoryDecision& decision, bool& outWasAdded) const;
    std::vector<memoryEntry> LoadMemories() const;
    std::vector<memoryEntry> LoadMissingEmbeddings(
        const std::string& embeddingModel,
        std::size_t maxEntries = 25) const;
    bool SaveEmbedding(
        const std::string& memoryId,
        const std::string& embeddingModel,
        const std::vector<float>& embedding) const;
private:
    longTermMemory store;
};
