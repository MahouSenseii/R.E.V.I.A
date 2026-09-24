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

// Exact nonempty text is Duplicate regardless of the embedding. All nonidentical
// summaries are retained: token overlap and embeddings cannot prove entity identity,
// argument order, modality, quantification or negation scope. Save separately supports
// its established case/whitespace formatting normalization without semantic merging.
// Paraphrase accumulation and contradiction supersession remain unresolved; see
// ISSUE-REVIA-0079 and ISSUE-REVIA-0080. Neither older nor incoming claims are discarded
// on an uncertain semantic judgment.
[[nodiscard]] MemoryRelation ClassifyRelation(
    const std::string& existingSummary,
    const std::string& candidateSummary,
    float similarity,
    const ReconciliationSettings& settings = {});

} // namespace revia::memory
