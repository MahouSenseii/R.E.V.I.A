#include "Core/memoryManager.h"

#include <utility>

memoryManager::memoryManager() = default;

memoryManager::memoryManager(std::string databasePath) : store(std::move(databasePath)) {}

memoryManager::~memoryManager() = default;

bool memoryManager::SaveAutomaticMemory(const memoryDecision& decision, bool& outWasAdded) const
{
    return store.Save(decision, outWasAdded);
}

std::vector<memoryEntry> memoryManager::LoadMemories() const
{
    return store.Load();
}

std::vector<memoryEntry> memoryManager::LoadMissingEmbeddings(
    const std::string& embeddingModel,
    const std::size_t maxEntries) const
{
    return store.LoadMissingEmbeddings(embeddingModel, maxEntries);
}

bool memoryManager::SaveEmbedding(
    const std::string& memoryId,
    const std::string& embeddingModel,
    const std::vector<float>& embedding) const
{
    return store.SaveEmbedding(memoryId, embeddingModel, embedding);
}
