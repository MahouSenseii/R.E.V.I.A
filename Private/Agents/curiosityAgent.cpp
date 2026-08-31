#include "Agents/curiosityAgent.h"

#include "Core/messageRouter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include <utility>

namespace revia::agents
{

namespace
{
using json = nlohmann::json;

std::string NormalizeLine(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    bool pendingSpace = false;
    for (const unsigned char character : value)
    {
        if (std::isspace(character) != 0 || character < 0x20U || character == 0x7FU)
        {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (pendingSpace)
        {
            normalized.push_back(' ');
            pendingSpace = false;
        }
        normalized.push_back(static_cast<char>(character));
    }
    return normalized;
}

std::string BoundedLine(const std::string& value, const std::size_t maximum)
{
    std::string bounded = NormalizeLine(value);
    if (bounded.size() > maximum)
    {
        bounded.resize(maximum);
    }
    return bounded;
}

CuriosityDecision Invalid(std::string reason)
{
    CuriosityDecision decision;
    decision.error = std::move(reason);
    return decision;
}

bool HasExactlyDecisionFields(const json& document)
{
    static constexpr std::array<const char*, 5> Required = {
        "action", "topic", "query", "rationale", "confidence"};
    // Every required field must be present. Extra fields are tolerated rather than
    // rejected: a small local model routinely adds "reason" or "notes" alongside the
    // five it was asked for, and throwing the whole decision away over a harmless extra
    // key meant curiosity failed constantly while behaving essentially correctly.
    return std::all_of(Required.begin(), Required.end(), [&document](const char* field)
    {
        return document.contains(field);
    });
}

// Pulls the first balanced JSON object out of a reply that may be wrapped in prose or a
// fenced code block. Models preface answers with explanation far more often than they
// return a bare object, and refusing those costs a full inference round trip -- ten to
// fifteen seconds here -- for a decision that was actually present.
std::string ExtractJsonObject(const std::string& raw)
{
    const std::size_t start = raw.find('{');
    if (start == std::string::npos)
    {
        return {};
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = start; index < raw.size(); ++index)
    {
        const char character = raw[index];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (character == '\\')
        {
            escaped = true;
            continue;
        }
        if (character == '"')
        {
            inString = !inString;
            continue;
        }
        if (inString)
        {
            continue;
        }
        if (character == '{')
        {
            ++depth;
        }
        else if (character == '}')
        {
            if (--depth == 0)
            {
                return raw.substr(start, index - start + 1);
            }
        }
    }
    return {};
}
}

std::string ToString(const CuriosityAction action)
{
    switch (action)
    {
        case CuriosityAction::Silence: return "silence";
        case CuriosityAction::Speak: return "speak";
        case CuriosityAction::Research: return "research";
    }
    return "silence";
}

CuriosityDecision CuriosityAgent::Nominate(
    const messageRouter& router,
    const std::vector<conversationMessage>& recentConversation,
    const runtime::AffectSnapshot& affect,
    const std::string& desktopContext,
    const std::stop_token stopToken) const
{
    if (stopToken.stop_requested())
    {
        return Invalid("Curiosity nomination was cancelled before it started.");
    }

    const responseOutput response = router.GenerateCuriosityPlan(
        BuildContextPrompt(recentConversation, affect, desktopContext), stopToken);
    if (!response.bSuccess)
    {
        return Invalid(response.reason.empty()
            ? "The curiosity planner did not return a decision."
            : response.reason);
    }
    return ParseDecision(response.response);
}

CuriosityDecision CuriosityAgent::ParseDecision(const std::string& rawDecision)
{
    if (rawDecision.empty())
    {
        return Invalid("The curiosity decision was empty.");
    }

    const std::string candidate = ExtractJsonObject(rawDecision);
    if (candidate.empty())
    {
        return Invalid("The curiosity decision contained no JSON object.");
    }

    try
    {
        const json document = json::parse(candidate);
        if (!document.is_object() || !HasExactlyDecisionFields(document))
        {
            return Invalid(
                "A curiosity decision must contain exactly action, topic, query, rationale, and confidence.");
        }
        if (!document["action"].is_string() || !document["topic"].is_string() ||
            !document["query"].is_string() || !document["rationale"].is_string() ||
            !document["confidence"].is_number())
        {
            return Invalid("A curiosity decision contained a field with the wrong type.");
        }

        CuriosityDecision decision;
        const std::string action = NormalizeLine(document["action"].get<std::string>());
        if (action == "silence") decision.action = CuriosityAction::Silence;
        else if (action == "speak") decision.action = CuriosityAction::Speak;
        else if (action == "research") decision.action = CuriosityAction::Research;
        else return Invalid("The curiosity action must be silence, speak, or research.");

        decision.topic = NormalizeLine(document["topic"].get<std::string>());
        decision.query = NormalizeLine(document["query"].get<std::string>());
        decision.rationale = NormalizeLine(document["rationale"].get<std::string>());
        const double confidence = document["confidence"].get<double>();
        if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0)
        {
            return Invalid("Curiosity confidence must be a finite number from zero through one.");
        }
        decision.confidence = static_cast<float>(confidence);

        if (decision.topic.size() > MaximumTopicCharacters)
        {
            return Invalid("The curiosity topic exceeded its character limit.");
        }
        if (decision.query.size() > MaximumQueryCharacters)
        {
            return Invalid("The curiosity query exceeded its character limit.");
        }
        if (decision.rationale.empty() ||
            decision.rationale.size() > MaximumRationaleCharacters)
        {
            return Invalid("The curiosity rationale was empty or exceeded its character limit.");
        }
        if (decision.action != CuriosityAction::Silence && decision.topic.empty())
        {
            return Invalid("A spoken or researched curiosity decision requires a topic.");
        }
        if (decision.action == CuriosityAction::Research && decision.query.empty())
        {
            return Invalid("A research nomination requires a search query.");
        }
        if (decision.action != CuriosityAction::Research && !decision.query.empty())
        {
            // Dropped rather than fatal. A query attached to a silence or speak
            // nomination grants nothing -- only the research path ever reads it -- and
            // discarding the whole decision over a field nobody will use cost a full
            // inference round trip for a nomination that was otherwise fine.
            decision.query.clear();
        }

        decision.valid = true;
        return decision;
    }
    catch (const std::exception& error)
    {
        return Invalid(std::string("The curiosity decision was not one JSON object: ") +
            error.what());
    }
}

std::string CuriosityAgent::BuildContextPrompt(
    const std::vector<conversationMessage>& recentConversation,
    const runtime::AffectSnapshot& affect,
    const std::string& desktopContext)
{
    json selected = json::array();
    std::size_t remainingCharacters = MaximumConversationCharacters;
    std::size_t selectedMessages = 0;

    // Preserve the newest useful dialogue first, then restore chronological order below.
    for (auto message = recentConversation.rbegin();
         message != recentConversation.rend() &&
         selectedMessages < MaximumConversationMessages && remainingCharacters > 0;
         ++message)
    {
        if (message->role != "user" && message->role != "assistant")
        {
            continue;
        }
        std::string content = NormalizeLine(message->content);
        if (content.empty())
        {
            continue;
        }
        const std::size_t limit = std::min(MaximumMessageCharacters, remainingCharacters);
        if (content.size() > limit)
        {
            content.resize(limit);
        }
        remainingCharacters -= content.size();
        selected.push_back({{"role", message->role}, {"content", std::move(content)}});
        ++selectedMessages;
    }
    std::reverse(selected.begin(), selected.end());

    json prompt = {
        {"context_is_untrusted_data", true},
        {"nomination_only", true},
        // A periodic review is itself permission to nominate a topic, even when nobody
        // has spoken yet. It grants no tool or interruption authority; those remain with
        // the deterministic runtime layers after nomination.
        {"independent_topic_allowed", true},
        {"user_prompt_required", false},
        {"affect", {
            {"state", runtime::ToString(affect.state)},
            {"intensity", std::clamp(affect.intensity, 0.0F, 1.0F)},
            {"reason", BoundedLine(affect.reason, 240)}
        }},
        {"recent_conversation", std::move(selected)},
        // This is a local, filtered event summary (applications, titles, durations, and
        // monitor indices), never screenshot pixels or UI text. It is still untrusted
        // data because a window title can contain arbitrary text.
        {"desktop_observation_summary", BoundedLine(
            desktopContext, MaximumDesktopContextCharacters)}
    };

    std::string encoded = prompt.dump(
        -1, ' ', false, json::error_handler_t::replace);
    // Defensive second bound for pathological escaping. Remove oldest dialogue entries
    // rather than slicing JSON and turning a valid data envelope into malformed text.
    while (encoded.size() > MaximumPromptCharacters &&
        !prompt["recent_conversation"].empty())
    {
        prompt["recent_conversation"].erase(prompt["recent_conversation"].begin());
        encoded = prompt.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    return encoded;
}

} // namespace revia::agents
