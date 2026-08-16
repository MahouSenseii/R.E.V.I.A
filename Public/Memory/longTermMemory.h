#pragma once

#include "Library/structLibrary.h"

#include <cstddef>
#include <string>
#include <vector>


class longTermMemory
{
public:
    explicit longTermMemory(std::string path = "Memory/revia_memory.db");
    ~longTermMemory();

    std::vector<memoryEntry> Load() const;
    bool Save(const memoryDecision& decision, bool& outWasAdded) const;
    bool HasMemories() const;
    std::vector<memoryEntry> Search(
        const std::string& query,
        std::size_t maxEntries = 6,
        const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "") const;
    std::string BuildPromptBlock(
        const std::string& query = "",
        std::size_t maxEntries = 6,
        const std::vector<float>& queryEmbedding = {},
        const std::string& embeddingModel = "") const;
    std::vector<memoryEntry> LoadMissingEmbeddings(
        const std::string& embeddingModel,
        std::size_t maxEntries = 25) const;
    bool SaveEmbedding(
        const std::string& memoryId,
        const std::string& embeddingModel,
        const std::vector<float>& embedding) const;

private:

    std::string memoryPath;
};
