#include "Memory/semanticDuplicate.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryReconciliation.h"
#include "Memory/sensitiveContent.h"
#include "Core/utf8.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace revia::memory
{
namespace
{
std::vector<std::string> Words(const std::string& text)
{
    std::vector<std::string> words;
    std::string word;
    for (const unsigned char ch : text)
    {
        if (std::isalnum(ch) || ch == '\'') word.push_back(static_cast<char>(std::tolower(ch)));
        else if (!word.empty()) { words.push_back(std::move(word)); word.clear(); }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

std::vector<std::string> ProtectedWords(const std::vector<std::string>& words)
{
    static const std::unordered_set<std::string> protectedWords = {
        "always", "usually", "generally", "sometimes", "occasionally", "all", "any", "only", "most", "some",
        "may", "might", "must", "can", "could", "will", "would", "should", "if", "when", "unless", "while",
        "before", "after", "during", "since", "until", "possibly", "probably", "thinks", "believes", "reports",
        "says", "claimed", "heard", "uncertain", "maybe", "now", "currently", "once", "twice", "daily", "weekly",
        "morning", "evening", "night", "weekends", "today", "yesterday", "tomorrow", "hours", "minutes", "seconds",
        "grams", "milligrams", "kilograms", "meters", "kilometers", "miles", "dollars", "euros", "percent",
        "mother", "father", "sister", "brother", "wife", "husband", "daughter", "son", "user", "assistant",
        "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
    static const std::unordered_map<std::string, std::string> groups = {
        {"each", "every"}, {"every", "every"}, {"short", "concise"}, {"brief", "concise"}, {"concise", "concise"},
        {"long", "detailed"}, {"detailed", "detailed"}, {"verbose", "detailed"}, {"dark", "dark"}, {"light", "light"},
        {"hot", "hot"}, {"cold", "cold"}, {"enabled", "enabled"}, {"disabled", "disabled"},
        {"public", "public"}, {"private", "private"}, {"local", "local"}, {"remote", "remote"},
        {"increase", "increase"}, {"decrease", "decrease"}, {"true", "true"}, {"false", "false"}};
    std::vector<std::string> result;
    for (const auto& word : words)
    {
        if (const auto found = groups.find(word); found != groups.end()) result.push_back(found->second);
        else if (protectedWords.contains(word)) result.push_back(word);
    }
    return result;
}

bool NegativeOrChanged(const std::vector<std::string>& words)
{
    static const std::unordered_set<std::string> markers = {"not", "no", "never", "cannot", "dislikes", "dislike",
        "disliked", "hates", "hate", "hated", "avoids", "avoid", "stopped", "stop", "quit", "longer", "instead",
        "switched", "changed", "moved", "used", "former", "formerly", "previously", "anymore", "rather"};
    return std::any_of(words.begin(), words.end(), [&](const auto& word)
        { return markers.contains(word) || word.ends_with("n't"); });
}

bool StrictSubset(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    const std::set<std::string> left(a.begin(), a.end()), right(b.begin(), b.end());
    return left.size() < right.size() && std::includes(right.begin(), right.end(), left.begin(), left.end());
}
}

bool ValidSemanticDuplicateSettings(const SemanticDuplicateSettings& settings)
{
    return settings.enabled && !settings.modelIdentity.empty() && settings.modelIdentity.size() <= 256 &&
        revia::utf8::IsValid(settings.modelIdentity) &&
        std::none_of(settings.modelIdentity.begin(), settings.modelIdentity.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }) &&
        std::isfinite(settings.minimumCandidateSimilarity) && settings.minimumCandidateSimilarity >= 0.80F &&
        settings.minimumCandidateSimilarity <= 1.0F && settings.minimumEntailment == 0.98 && settings.maximumContradiction == 0.01 &&
        settings.maximumCandidates > 0 && settings.maximumCandidates <= 4;
}

bool MayCompareSemanticDuplicates(const std::string& left, const std::string& right)
{
    if (left.empty() || right.empty() || left.size() > 1024 || right.size() > 1024 ||
        !revia::utf8::IsValid(left) || !revia::utf8::IsValid(right) ||
        ContainsSensitiveContent(left) || ContainsSensitiveContent(right) ||
        ClassifyRelation(left, right, 1.0F) != MemoryRelation::Refinement)
        return false;
    const auto a = Words(left), b = Words(right);
    if (a.empty() || b.empty() || a.size() > 128 || b.size() > 128 || a == b || NegativeOrChanged(a) || NegativeOrChanged(b) ||
        ProtectedWords(a) != ProtectedWords(b) || StrictSubset(a, b) || StrictSubset(b, a))
        return false;
    // A changed order with the same words can reverse subjects, preferences or
    // sequence. Conservatively retain it without asking a model to authorize a mark.
    auto sortedA = a, sortedB = b;
    std::sort(sortedA.begin(), sortedA.end());
    std::sort(sortedB.begin(), sortedB.end());
    return sortedA != sortedB;
}

bool IsStrongBidirectionalEntailment(const NliPairResult& result, const SemanticDuplicateSettings& settings)
{
    if (!ValidSemanticDuplicateSettings(settings) || !result.succeeded || result.truncated || result.modelIdentity != settings.modelIdentity)
        return false;
    for (double value : {result.forwardEntailment, result.reverseEntailment, result.forwardContradiction, result.reverseContradiction})
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) return false;
    return result.forwardEntailment >= settings.minimumEntailment && result.reverseEntailment >= settings.minimumEntailment &&
        result.forwardContradiction <= settings.maximumContradiction && result.reverseContradiction <= settings.maximumContradiction &&
        result.forwardEntailment + result.forwardContradiction <= 1.000001 &&
        result.reverseEntailment + result.reverseContradiction <= 1.000001;
}

SemanticDuplicateCheckResult CheckSemanticDuplicates(longTermMemory& store, const std::string& memoryId,
    const std::string& embeddingModel, const MemoryScope& scope, const SemanticDuplicateSettings& settings,
    SemanticDuplicateScorer* scorer, std::stop_token stopToken, std::function<bool()> admission)
{
    SemanticDuplicateCheckResult checked;
    const auto admitted = [&]() { try { return !stopToken.stop_requested() && (!admission || admission()); } catch (...) { return false; } };
    if (!scorer || !ValidSemanticDuplicateSettings(settings) || !IsAttributedPrivateScope(scope) || !admitted()) return checked;
    const auto candidates = store.SemanticDuplicateCandidates(memoryId, embeddingModel, scope, settings);
    for (const auto& pair : candidates)
    {
        if (!admitted()) break;
        if (!MayCompareSemanticDuplicates(pair.candidate.summary, pair.existing.summary)) continue;
        ++checked.considered;
        NliPairResult verdict;
        try { verdict = scorer->Compare(pair.candidate.summary, pair.existing.summary, stopToken); }
        catch (...) { break; }
        if (!admitted()) break;
        if (IsStrongBidirectionalEntailment(verdict, settings) && store.SaveSemanticDuplicateMark(pair, verdict, scope, settings, admitted))
            ++checked.marked;
    }
    return checked;
}
}
