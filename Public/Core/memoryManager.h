#pragma once

#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"

#include <string>

class memoryManager
{
public:
    memoryManager();
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
