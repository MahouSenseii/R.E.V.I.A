#include "Memory/memoryReconciliation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace revia::memory
{

namespace
{

std::vector<std::string> Tokenize(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (const unsigned char character : text)
    {
        // Punctuation that lives inside a token is kept, because it is exactly what
        // makes "C++" different from "C" and "1.2.3" different from "123".
        const bool separator = std::isspace(character) != 0 || character == ',' ||
            character == ';' || character == ':' || character == '"' ||
            character == '(' || character == ')';
        if (separator)
        {
            if (!current.empty()) tokens.push_back(std::exchange(current, {}));
            continue;
        }
        current.push_back(static_cast<char>(character));
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

// Sentence-final punctuation, removed. Punctuation *inside* the token stays, because
// that is what a discriminator is made of.
std::string TrimSentencePunctuation(std::string token)
{
    while (!token.empty() &&
        (token.back() == '.' || token.back() == '!' || token.back() == '?' ||
         token.back() == ',') &&
        // ".NET" and "1.2.3" keep theirs: the stop is not final if something
        // alphanumeric follows an earlier one.
        token.find_first_of(".!?,") == token.size() - 1)
    {
        token.pop_back();
    }
    return token;
}

bool HasDigit(const std::string& token)
{
    return std::any_of(token.begin(), token.end(),
        [](const unsigned char character) { return std::isdigit(character) != 0; });
}

bool HasInnerPunctuation(const std::string& token)
{
    return std::any_of(token.begin(), token.end(),
        [](const unsigned char character)
        {
            return std::isalnum(character) == 0 && character != '\'';
        });
}

bool IsCapitalised(const std::string& token)
{
    return !token.empty() && std::isupper(static_cast<unsigned char>(token.front())) != 0;
}

// Identifier-like tokens used only for diagnostic relation hints.
//
// The first token is excluded from the proper-noun rule because every summary starts
// with a capital letter by construction -- "The user ..." / "Revia ..." -- and treating
// that as a name would put a meaningless entry at the head of every sequence.
std::vector<std::string> Discriminators(const std::string& text)
{
    const std::vector<std::string> tokens = Tokenize(text);
    std::vector<std::string> found;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        const std::string trimmed = TrimSentencePunctuation(tokens[index]);
        if (trimmed.empty()) continue;
        if (HasDigit(trimmed) || HasInnerPunctuation(trimmed) ||
            (index > 0 && IsCapitalised(trimmed)))
        {
            found.push_back(Lowered(trimmed));
        }
    }
    return found;
}

// Words that say the claim is negative, or that it has changed. One of these on one
// side and not the other is the difference between a repetition and a correction.
bool CarriesPolarityShift(const std::string& text)
{
    static constexpr std::array<std::string_view, 26> Markers = {
        "not", "no", "never", "cannot", "dislikes", "dislike", "disliked",
        "hates", "hate", "hated", "avoids", "avoid", "stopped", "stop", "quit",
        "longer", "instead", "switched", "changed", "moved", "used", "former",
        "formerly", "previously", "anymore", "rather"
    };
    for (const std::string& raw : Tokenize(text))
    {
        const std::string token = Lowered(TrimSentencePunctuation(raw));
        if (token.empty()) continue;
        if (std::find(Markers.begin(), Markers.end(), token) != Markers.end())
        {
            return true;
        }
        // Contracted negations: doesn't, won't, isn't, can't.
        if (token.size() > 3 && token.compare(token.size() - 3, 3, "n't") == 0)
        {
            return true;
        }
    }
    return false;
}

} // namespace

std::string ToString(const MemoryRelation value)
{
    switch (value)
    {
        case MemoryRelation::Duplicate: return "duplicate";
        case MemoryRelation::Refinement: return "refinement";
        case MemoryRelation::Contradiction: return "contradiction";
        case MemoryRelation::Unrelated:
        default: return "unrelated";
    }
}

MemoryRelation ClassifyRelation(
    const std::string& existingSummary,
    const std::string& candidateSummary,
    const float similarity,
    const ReconciliationSettings& settings)
{
    // Text identity establishes equivalence independently of vector quality. A
    // shared set of words cannot establish subject, order, scope or modality.
    if (!existingSummary.empty() && existingSummary == candidateSummary)
    {
        return MemoryRelation::Duplicate;
    }
    if (existingSummary.empty() || candidateSummary.empty() ||
        !std::isfinite(similarity) || similarity < settings.relatedSimilarity)
    {
        return MemoryRelation::Unrelated;
    }

    // These remaining labels are diagnostic hints only. None proves equivalence
    // or authorizes discarding either the existing or the incoming claim.
    if (CarriesPolarityShift(existingSummary) != CarriesPolarityShift(candidateSummary))
    {
        return MemoryRelation::Contradiction;
    }

    // Keep identifier-like tokens in order when choosing a diagnostic label.
    if (Discriminators(existingSummary) != Discriminators(candidateSummary))
    {
        return MemoryRelation::Unrelated;
    }

    // Similarity can suggest related wording; it cannot prove added specificity.
    return similarity >= settings.duplicateSimilarity ? MemoryRelation::Refinement
                                                      : MemoryRelation::Unrelated;
}

} // namespace revia::memory
