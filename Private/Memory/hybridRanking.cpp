#include "Memory/hybridRanking.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace revia::memory
{

std::string BuildFtsQuery(const std::string& query)
{
    static const std::unordered_set<std::string> StopWords = {
        "a", "an", "and", "are", "about", "can", "could", "did", "do", "does", "for",
        "how", "i", "in", "is", "it", "me", "my", "of", "on", "or", "should", "that",
        "the", "this", "to", "user", "what", "when", "where", "which", "who", "why", "with",
        "would", "you", "your"
    };

    std::vector<std::string> tokens;
    std::unordered_set<std::string> seen;
    std::string token;
    const auto finishToken = [&]()
    {
        if (token.size() >= 2 && !StopWords.contains(token) && seen.insert(token).second)
        {
            tokens.push_back(token);
        }
        token.clear();
    };

    for (const unsigned char character : query)
    {
        if (std::isalnum(character))
        {
            token.push_back(static_cast<char>(std::tolower(character)));
        }
        else
        {
            finishToken();
        }
    }
    finishToken();

    std::ostringstream expression;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        if (index > 0)
        {
            expression << " OR ";
        }
        expression << '"' << tokens[index] << "\"*";
    }
    return expression.str();
}

bool IsValidEmbedding(const std::vector<float>& embedding)
{
    return !embedding.empty() && std::all_of(embedding.begin(), embedding.end(), [](const float value) { return std::isfinite(value); });
}

double VectorNorm(const std::vector<float>& vector)
{
    double length = 0.0;
    for (const float value : vector)
    {
        length += static_cast<double>(value) * value;
    }
    return std::sqrt(length);
}

std::optional<float> CosineSimilarity(
    const std::vector<float>& query, const double queryNorm, const void* blob, const int byteCount, const std::int64_t dimensions)
{
    if (queryNorm <= 0.0 || blob == nullptr || dimensions != static_cast<std::int64_t>(query.size()) ||
        byteCount != static_cast<int>(query.size() * sizeof(float)))
    {
        return std::nullopt;
    }
    const auto* bytes = static_cast<const unsigned char*>(blob);
    double dot = 0.0;
    double storedLength = 0.0;
    for (std::size_t index = 0; index < query.size(); ++index)
    {
        // Copied out one value at a time: a SQLite blob carries no alignment promise.
        float stored = 0.0F;
        std::memcpy(&stored, bytes + index * sizeof(float), sizeof(float));
        dot += static_cast<double>(query[index]) * stored;
        storedLength += static_cast<double>(stored) * stored;
    }
    if (storedLength <= 0.0)
    {
        return std::nullopt;
    }
    const auto similarity = static_cast<float>(dot / (queryNorm * std::sqrt(storedLength)));
    return std::isfinite(similarity) ? std::optional<float>(similarity) : std::nullopt;
}

std::vector<FusedRank> FuseReciprocalRanks(const std::vector<RankedList>& lists)
{
    std::vector<FusedRank> fused;
    std::unordered_map<std::string, std::size_t> positions;
    for (const RankedList& list : lists)
    {
        for (std::size_t rank = 0; rank < list.keys.size(); ++rank)
        {
            const double score = list.weight / (ReciprocalRankConstant + static_cast<double>(rank) + 1.0);
            const auto [found, inserted] = positions.emplace(list.keys[rank], fused.size());
            if (inserted)
            {
                fused.push_back({list.keys[rank], score, fused.size()});
            }
            else
            {
                fused[found->second].score += score;
            }
        }
    }
    return fused;
}

} // namespace revia::memory
