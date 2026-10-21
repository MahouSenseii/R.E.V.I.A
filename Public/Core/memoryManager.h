#pragma once

#include "Memory/memoryTypes.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryRevision.h"
#include "Memory/memoryCorrection.h"

#include <functional>
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

    bool SaveAutomaticMemory(
        const memoryDecision& decision, bool& outWasAdded, std::string* outMemoryId = nullptr, std::function<bool()> admission = {}) const;
    bool SaveOwnerRevision(const revia::memory::MemoryRevisionRequest& request, revia::memory::MemoryRevisionReceipt& outReceipt,
        std::string& outError, std::function<bool()> admission = {},
        revia::memory::MemoryRevisionScopeAdmission acquireReviewScope = {}) const;
    std::vector<revia::memory::MemoryCorrectionProposal> ProposeCorrections(const std::string& savedMemoryId,
        const memoryDecision& decision, const std::string& input, const revia::memory::MemoryScope& scope,
        std::uint64_t turnId, std::function<bool()> admission = {}, std::stop_token stopToken = {}) const;
    std::vector<memoryEntry> LoadMemories() const;
    std::vector<memoryEntry> LoadMissingEmbeddings(const std::string& embeddingModel, std::size_t maxEntries = 25) const;
    EmbeddingBackfillPage ScanMissingEmbeddings(const std::string& embeddingModel,
        std::int64_t afterRowId, std::size_t maxEntries = 25) const;
    bool NeedsEmbedding(const std::string& memoryId, const std::string& embeddingModel) const;
    bool SaveEmbedding(const std::string& memoryId, const std::string& embeddingModel, const std::vector<float>& embedding) const;
    revia::memory::SemanticDuplicateCheckResult CheckSemanticDuplicates(const std::string& memoryId,
        const std::string& embeddingModel, const revia::memory::MemoryScope& scope,
        const revia::memory::SemanticDuplicateSettings& settings, revia::memory::SemanticDuplicateScorer* scorer,
        std::stop_token stopToken = {}, std::function<bool()> admission = {});
private:
    longTermMemory store;
};
