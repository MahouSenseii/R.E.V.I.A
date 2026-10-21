#include "Memory/memoryTypes.h"
#include "Core/memoryManager.h"

#include <utility>

memoryManager::memoryManager() = default;

memoryManager::memoryManager(std::string databasePath) : store(std::move(databasePath)) {}

memoryManager::~memoryManager() = default;

bool memoryManager::SaveAutomaticMemory(
    const memoryDecision& decision, bool& outWasAdded, std::string* outMemoryId, std::function<bool()> admission) const
{
    return store.Save(decision, outWasAdded, outMemoryId, std::move(admission));
}

std::vector<memoryEntry> memoryManager::LoadMemories() const
{
    return store.Load();
}

bool memoryManager::SaveOwnerRevision(const revia::memory::MemoryRevisionRequest& request, revia::memory::MemoryRevisionReceipt& receipt,
    std::string& error, std::function<bool()> admission, revia::memory::MemoryRevisionScopeAdmission acquireReviewScope) const
{
    return store.SaveOwnerRevision(request, receipt, error, std::move(admission), std::move(acquireReviewScope));
}

std::vector<revia::memory::MemoryCorrectionProposal> memoryManager::ProposeCorrections(const std::string& id,
    const memoryDecision& decision, const std::string& input, const revia::memory::MemoryScope& scope,
    std::uint64_t turnId, std::function<bool()> admission, std::stop_token stopToken) const
{
    return revia::memory::ProposeMemoryCorrections(store, id, decision, input, scope, turnId, std::move(admission), stopToken);
}

std::vector<memoryEntry> memoryManager::LoadMissingEmbeddings(const std::string& embeddingModel, const std::size_t maxEntries) const
{
    return store.LoadMissingEmbeddings(embeddingModel, maxEntries);
}

EmbeddingBackfillPage memoryManager::ScanMissingEmbeddings(const std::string& model, std::int64_t afterRowId, std::size_t maxEntries) const
{
    return store.ScanMissingEmbeddings(model, afterRowId, maxEntries);
}

bool memoryManager::NeedsEmbedding(const std::string& id, const std::string& model) const
{
    return store.NeedsEmbedding(id, model);
}

bool memoryManager::SaveEmbedding(const std::string& memoryId, const std::string& embeddingModel, const std::vector<float>& embedding) const
{
    return store.SaveEmbedding(memoryId, embeddingModel, embedding);
}

revia::memory::SemanticDuplicateCheckResult memoryManager::CheckSemanticDuplicates(const std::string& id,
    const std::string& model, const revia::memory::MemoryScope& scope, const revia::memory::SemanticDuplicateSettings& settings,
    revia::memory::SemanticDuplicateScorer* scorer, std::stop_token stopToken, std::function<bool()> admission)
{
    return revia::memory::CheckSemanticDuplicates(store, id, model, scope, settings, scorer, stopToken, std::move(admission));
}
