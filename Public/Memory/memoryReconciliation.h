#pragma once

#include <string>

namespace revia::memory
{

// Except for Duplicate, these are diagnostic hints, not established semantic facts.
// They never authorize discarding or superseding a durable claim.
enum class MemoryRelation
{
    Unrelated,
    // Nonempty, byte-identical summary text.
    Duplicate,
    // A highly similar candidate. Equivalence and added specificity are unproven.
    Refinement,
    // Different polarity/change markers. Scope and subject identity are unproven.
    Contradiction
};

[[nodiscard]] std::string ToString(MemoryRelation value);

struct ReconciliationSettings
{
    // Legacy name: separates diagnostic Refinement from Unrelated, never deduplicates.
    float duplicateSimilarity = 0.93F;
    // Below this, distinct text receives no further diagnostic classification.
    float relatedSimilarity = 0.80F;
};

// Exact nonempty text is Duplicate; nonidentical summaries are retained regardless of vector/token overlap.
// Save normalizes case/whitespace only; uncertain paraphrases and contradictions never discard claims.
[[nodiscard]] MemoryRelation ClassifyRelation(const std::string& existingSummary,
    const std::string& candidateSummary, float similarity, const ReconciliationSettings& settings = {});

} // namespace revia::memory
