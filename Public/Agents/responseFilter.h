#pragma once

#include <string>

namespace revia::agents
{

// Verified, non-secret state supplied by the application. User text cannot mutate this
// structure, so both filter layers can distinguish a real setting from a conversational
// claim that merely says a setting changed.
struct ResponseFilterContext
{
    bool internetStateKnown = false;
    bool internetEnabled = false;
    bool automaticInternetLookup = false;
    bool visibleBrowser = false;
    bool autonomousInternetResearch = false;
    bool internetTopicIsActive = false;
    std::string internetProvider = "approved provider";

    // Populated only after a real screen capture or retained ambient observation was
    // successfully described. The hard filter uses it to stop a model prior such as
    // "assistants cannot see screens" from contradicting evidence the runtime supplied.
    bool screenTopicIsActive = false;
    bool screenObservationAvailable = false;
    std::string screenObservation;

    [[nodiscard]] std::string Describe() const;
};

struct HardFilterResult
{
    std::string text;
    bool changed = false;
    bool blocked = false;
    std::string reason = "Hard response filter passed.";
};

struct AiFilterDecision
{
    bool parsed = false;
    bool replace = false;
    std::string replacement;
    std::string reason;
};

// Pure parsing and deterministic enforcement. The LLM call remains owned by the LLM
// service, which keeps this class testable and prevents a safety rule from owning an
// inference process.
class ResponseFilter
{
public:
    [[nodiscard]] HardFilterResult ApplyHard(
        const std::string& userInput,
        const std::string& candidate,
        const ResponseFilterContext& context,
        int maxCharacters) const;
    [[nodiscard]] AiFilterDecision ParseAiDecision(const std::string& jsonText) const;
};

} // namespace revia::agents
