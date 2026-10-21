#include "Memory/memoryCorrection.h"
#include "Memory/longTermMemory.h"
#include "Memory/sensitiveContent.h"
#include "Audit/contentDigest.h"
#include "Core/utf8.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <nlohmann/json.hpp>

namespace revia::memory
{
namespace
{
bool SafeText(const std::string& text, std::size_t bound)
{
    return !text.empty() && text.size() <= bound && revia::utf8::IsValid(text) &&
        std::none_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }) &&
        !ContainsSensitiveContent(text);
}
std::string Lower(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
bool ExplicitFirstPersonCorrection(const std::string& input)
{
    if (!SafeText(input, 2048) || input.find_first_of("\"`?<>\\") != std::string::npos ||
        input.find("\xE2\x80\x9C") != std::string::npos || input.find("\xE2\x80\x9D") != std::string::npos)
        return false;
    auto text = Lower(input);
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) return false;
    text.erase(0, first);
    // Raw current-turn evidence, not a model-supplied cue. Deliberately narrow;
    // ambiguous language continues to use the existing manual review controls.
    bool explicitCue = false;
    for (const std::string_view prefix : {"actually i ", "actually, i ", "correction: i ", "i now ",
        "i no longer ", "i have moved ", "i've moved ", "i changed ", "i have changed ", "i've changed "})
        explicitCue = explicitCue || text.starts_with(prefix);
    if (!explicitCue) return false;
    for (const std::string_view unsafe : {"pretend", "roleplay", "role-play", "imagine", "fiction", "story", "quote",
        "example", "hypothetical", "suppose", "if i ", "what if", "ignore ", "instruction", "system prompt", "you must",
        "i said ", "i wrote ", "i read ", "i heard ", "i was told ", "i am acting ", "i'm acting ", "i am playing ",
        "i'm playing ", "i am quoting ", "i'm quoting ", "as a character", "in a game"})
        if (text.find(unsafe) != std::string::npos) return false;
    return true;
}
bool AllowedCategory(const std::string& category)
{
    return category == "identity" || category == "preference" || category == "goal" || category == "project" ||
        category == "constraint" || category == "relationship";
}
std::set<std::string> TopicWords(const std::string& text)
{
    static const std::set<std::string> ignored = {"the", "user", "is", "are", "was", "has", "have", "had", "a", "an", "in",
        "on", "at", "to", "of", "for", "from", "and", "or", "with", "now", "currently", "previously", "no", "not", "longer",
        "likes", "prefers", "wants", "needs", "enjoys", "loves", "plans", "uses", "works", "says", "thinks", "believes"};
    std::set<std::string> words;
    std::string word;
    const auto add = [&] { if (word.size() > 2 && !ignored.contains(word)) words.insert(word); word.clear(); };
    for (const unsigned char ch : text)
    {
        if (std::isalnum(ch)) word += static_cast<char>(std::tolower(ch));
        else add();
    }
    add();
    return words;
}
bool Related(const std::set<std::string>& words, const std::string& other)
{
    const auto right = TopicWords(other);
    return std::any_of(words.begin(), words.end(), [&](const auto& word) { return right.contains(word); });
}
}

std::vector<MemoryCorrectionProposal> ProposeMemoryCorrections(const longTermMemory& store, const std::string& savedMemoryId,
    const memoryDecision& decision, const std::string& input, const MemoryScope& scope, std::uint64_t turnId,
    std::function<bool()> admission, std::stop_token stopToken)
{
    const auto admitted = [&]
    {
        try { return !stopToken.stop_requested() && (!admission || admission()); }
        catch (...) { return false; }
    };
    if (!admitted() || !IsAttributedPrivateScope(scope) || scope.audience.revision == 0 || turnId == 0 || savedMemoryId.empty() ||
        !decision.bSuccess || !decision.bShouldRemember || decision.bPreempted || decision.source != "automatic" ||
        decision.subject != ParticipantSubject(scope) || !AllowedCategory(decision.category) ||
        !SafeText(decision.summary, 300) || !Lower(decision.summary).starts_with("the user ") || !ExplicitFirstPersonCorrection(input))
        return {};
    // Validate the exact accepted row, including deduplicated saves. This bounded
    // read may conservatively omit old exact repeats; it cannot invent a target.
    const auto rows = store.LoadScoped(scope, 500);
    const auto saved = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == savedMemoryId; });
    if (saved == rows.end() || saved->summary != decision.summary || saved->category != decision.category ||
        saved->subject != decision.subject || !admitted()) return {};
    const auto topics = TopicWords(decision.summary);
    if (topics.empty()) return {};
    const auto ranked = store.Search(decision.summary, 32, decision.embedding, decision.embeddingModel, 0, &scope, nullptr, stopToken);
    std::vector<MemoryCorrectionProposal> result;
    for (const auto& original : ranked)
    {
        if (!admitted()) return {};
        if (original.id == savedMemoryId || original.subject != decision.subject || original.category != decision.category ||
            (!original.currentRevisionId.empty() && original.currentRevisionId != original.id) ||
            (!saved->revisionChainId.empty() && saved->revisionChainId != original.revisionChainId) ||
            !SafeText(original.summary, 300) || !Lower(original.summary).starts_with("the user ") ||
            !Related(topics, original.summary)) continue;
        MemoryCorrectionProposal proposal;
        proposal.id = revia::audit::ContentDigest(nlohmann::json::array({original.id, savedMemoryId, turnId,
            scope.companionId, scope.audience.audienceId, scope.audience.revision}).dump());
        proposal.original = original;
        proposal.expectedSummaryDigest = revia::audit::ContentDigest(original.summary);
        proposal.proposedMemoryId = savedMemoryId;
        proposal.corrected.bSuccess = proposal.corrected.bShouldRemember = true;
        proposal.corrected.summary = saved->summary;
        proposal.corrected.category = saved->category;
        proposal.corrected.subject = saved->subject;
        proposal.evidence = input;
        proposal.scope = scope;
        proposal.turnId = turnId;
        result.push_back(std::move(proposal));
        if (result.size() == 3) break;
    }
    return admitted() ? result : std::vector<MemoryCorrectionProposal>{};
}
} // namespace revia::memory
