#pragma once

#include "Memory/memoryTypes.h"
#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

class longTermMemory;

namespace revia::memory
{
struct NliPairResult
{
    bool succeeded = false;
    bool truncated = false;
    std::string modelIdentity;
    double forwardEntailment = 0.0;
    double reverseEntailment = 0.0;
    double forwardContradiction = 0.0;
    double reverseContradiction = 0.0;
};

// The session owns the optional local provider. Implementations must bound work,
// honor cancellation and reject truncation. Calling Compare never starts/downloads
// a model. Probabilities refer to explicit model entailment/contradiction labels.
class SemanticDuplicateScorer
{
public:
    virtual ~SemanticDuplicateScorer() = default;
    virtual NliPairResult Compare(const std::string& left, const std::string& right,
        std::stop_token stopToken = {}) = 0;
};

struct SemanticDuplicateSettings
{
    bool enabled = false;
    std::string modelIdentity;
    float minimumCandidateSimilarity = 0.80F;
    double minimumEntailment = 0.98;
    double maximumContradiction = 0.01;
    std::size_t maximumCandidates = 4;
};

struct SemanticDuplicatePair
{
    memoryEntry candidate;
    memoryEntry existing;
    float similarity = 0.0F;
    std::string embeddingModel;
};

// Advisory metadata only. Both immutable facts and all revision receipts remain
// authoritative; this mark never changes admission, ranking, or fact visibility.
struct SemanticDuplicateMark
{
    std::string memoryId;
    std::string matchedMemoryId;
    std::string memoryDigest;
    std::string matchedDigest;
    MemorySubject subject;
    std::string modelIdentity;
    double forwardEntailment = 0.0;
    double reverseEntailment = 0.0;
    std::string createdAt;
};

struct SemanticDuplicateCheckResult
{
    std::size_t considered = 0;
    std::size_t marked = 0;
};

[[nodiscard]] bool ValidSemanticDuplicateSettings(const SemanticDuplicateSettings& settings);
[[nodiscard]] bool MayCompareSemanticDuplicates(const std::string& left, const std::string& right);
[[nodiscard]] bool IsStrongBidirectionalEntailment(const NliPairResult& result,
    const SemanticDuplicateSettings& settings);

// Existing safety/subject/revision gates run before inference. Captured admission
// is checked again immediately before the store atomically appends a mark.
[[nodiscard]] SemanticDuplicateCheckResult CheckSemanticDuplicates(longTermMemory& store,
    const std::string& memoryId, const std::string& embeddingModel, const MemoryScope& scope,
    const SemanticDuplicateSettings& settings, SemanticDuplicateScorer* scorer,
    std::stop_token stopToken = {}, std::function<bool()> admission = {});
}
