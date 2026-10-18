#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace revia::memory
{

// The ranking pieces that long-term memory and the user's documents share: a lexical
// FTS5 query, cosine similarity over stored vectors, and reciprocal rank fusion of the
// ranked lists. One copy, so a fix to how recall ranks reaches both stores.

// An FTS5 MATCH expression: the distinct words of `query` (two characters or more,
// common stop words dropped) as quoted prefix terms joined by OR. Empty when nothing is
// left to match, which callers treat as "no lexical list".
[[nodiscard]] std::string BuildFtsQuery(const std::string& query);

[[nodiscard]] bool IsValidEmbedding(const std::vector<float>& embedding);
[[nodiscard]] double VectorNorm(const std::vector<float>& vector);

// Cosine similarity between `query` (whose norm the caller computed once) and a vector
// stored as raw floats. Empty when the stored vector has a different shape or no length,
// so a vector from another embedding model is skipped rather than compared.
[[nodiscard]] std::optional<float> CosineSimilarity(
    const std::vector<float>& query, double queryNorm, const void* blob, int byteCount, std::int64_t dimensions);

// Below this a semantic match is noise for nomic-embed-text and is not ranked at all.
inline constexpr float MinimumSemanticSimilarity = 0.35F;
inline constexpr double ReciprocalRankConstant = 60.0;

struct RankedList
{
    std::vector<std::string> keys;
    double weight = 1.0;
};

struct FusedRank
{
    std::string key;
    double score = 0.0;
    // The order in which the key first appeared across the lists, for a stable tie-break.
    std::size_t firstSeen = 0;
};

// Each list adds weight / (60 + rank + 1) to every key it holds. The result is in
// first-seen order; callers sort it with their own tie-break.
[[nodiscard]] std::vector<FusedRank> FuseReciprocalRanks(const std::vector<RankedList>& lists);

} // namespace revia::memory
