#include "LLM/contextFitting.h"

#include "Identity/promptMarkers.h"
#include "LLM/tokenEstimate.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace revia::llm
{

namespace
{
using json = nlohmann::json;

// Compacts the newest message without cutting the runtime block's markers, so the
// model can still tell the runtime's words from the user's. The user's words get at
// least half the budget; the block keeps whatever they do not need.
std::string CompactNewestMessage(const std::string& content, const std::size_t budget)
{
    namespace markers = revia::identity::markers;
    const std::string turnMarker = "\n[Earlier part of this turn compacted.]\n";
    const std::string open = std::string(markers::RuntimeTurnContext) + "\n";
    const std::string close = "\n" + std::string(markers::RuntimeTurnContextEnd) + "\n\n";
    const std::size_t closeAt = content.rfind(open, 0) == 0
        ? content.find(close, open.size()) : std::string::npos;
    if (closeAt == std::string::npos)
    {
        return CompactToTokenBudget(content, budget, turnMarker);
    }
    const std::string block = content.substr(open.size(), closeAt - open.size());
    const std::string user = content.substr(closeAt + close.size());
    if (budget <= open.size() + close.size())
    {
        return CompactToTokenBudget(user, budget, turnMarker);
    }
    const std::size_t available = budget - open.size() - close.size();
    const std::size_t userShare = std::min(user.size(),
        std::max(available / 2, available > block.size() ? available - block.size() : 0));
    return open + CompactToTokenBudget(
            block, available - userShare, "\n[Runtime context compacted.]\n") +
        close + CompactToTokenBudget(user, userShare, turnMarker);
}

// Shortens `text` until `count` says it fits `tokenBudget`.
//
// The compactors cut by bytes, and a byte budget always fits because bytes never
// under-count. With a real tokenizer that cuts four or five times more than it needs
// to, so the byte budget is first scaled by this text's own measured density and the
// result checked; the plain byte budget is the fallback when the check fails.
template <class Compactor>
std::string CompactCounted(
    const std::string& text,
    const std::size_t tokenBudget,
    const TokenCounter& count,
    Compactor compact)
{
    const std::size_t tokens = count(text);
    if (tokens <= tokenBudget) return text;
    if (tokens > 0 && text.size() > tokens)
    {
        // Nine tenths of the proportional share, so a text denser at one end than the
        // other still usually fits on the first try.
        const auto scaled = static_cast<std::size_t>(
            static_cast<double>(tokenBudget) * static_cast<double>(text.size()) /
            static_cast<double>(tokens) * 0.9);
        if (scaled > tokenBudget)
        {
            std::string candidate = compact(text, scaled);
            if (count(candidate) <= tokenBudget) return candidate;
        }
    }
    return compact(text, tokenBudget);
}
}

// Content costs come from `count` when it is supplied and from the byte-conservative
// allowance otherwise, with both per-message framing and a request-wide reserve. Custom
// templates may still exceed that reserve, so GenerateResponse has one context-specific
// recovery.
json BoundMessagesForContext(
    const json& messages,
    const int contextTokens,
    const int responseTokens,
    const std::size_t maximumPromptTokens,
    const TokenCounter& exactCount)
{
    if (!messages.is_array() || messages.empty())
    {
        return messages;
    }
    const auto usableTokens = static_cast<long long>(contextTokens) -
        responseTokens - ContextReserveTokens;
    if (usableTokens <= 0) return json::array();
    const std::size_t tokenBudget = std::min({
        static_cast<std::size_t>(usableTokens), maximumPromptTokens, MaximumPromptBytes});
    if (tokenBudget <= 2 * ChatTemplateTokensPerMessage)
        return json::array();
    // This is the text-only conversation path. An unexpected structured or
    // multimodal message must not receive a zero content cost and slip through.
    for (const auto& message : messages)
        if (!message.is_object() || !message.contains("content") ||
            !message["content"].is_string() || !message.contains("role") ||
            !message["role"].is_string()) return json::array();

    const auto costOf = [](const json& message, const TokenCounter& count)
    {
        const std::size_t content =
            message.contains("content") && message["content"].is_string()
                ? count(message["content"].get_ref<const std::string&>())
                : 0;
        return content + ChatTemplateTokensPerMessage;
    };

    // Bytes never under-count, so a request that fits by bytes fits whatever the
    // tokenizer says, and asking it would only add latency.
    const TokenCounter bytes = [](const std::string& text) { return EstimateTokens(text); };
    std::size_t byteTotal = 0;
    for (const auto& message : messages) byteTotal += costOf(message, bytes);
    if (byteTotal <= tokenBudget)
    {
        return messages;
    }

    const TokenCounter count = exactCount ? exactCount : bytes;
    const auto messageCost = [&costOf, &count](const json& message)
    {
        return costOf(message, count);
    };
    if (exactCount)
    {
        std::size_t total = 0;
        for (const auto& message : messages) total += messageCost(message);
        if (total <= tokenBudget)
        {
            return messages;
        }
    }

    json bounded = json::array();
    std::size_t used = 0;
    std::size_t firstDialogue = 0;
    if (messages.front().value("role", "") == "system")
    {
        json system = messages.front();
        const std::string content = system.value("content", "");
        const std::size_t systemBudget =
            (tokenBudget - 2 * ChatTemplateTokensPerMessage) * 7 / 10;
        system["content"] = CompactCounted(content, systemBudget, count,
            [](const std::string& text, const std::size_t budget)
            {
                return CompactToTokenBudget(
                    text, budget,
                    "\n\n[Older runtime context compacted to fit this model.]\n\n");
            });
        used = messageCost(system);
        bounded.push_back(std::move(system));
        firstDialogue = 1;
    }

    std::vector<json> recent;
    for (std::size_t index = messages.size(); index > firstDialogue; --index)
    {
        json message = messages[index - 1];
        const std::string content = message.value("content", "");
        if (content.empty())
        {
            continue;
        }
        const std::size_t remaining = tokenBudget > used ? tokenBudget - used : 0;
        if (remaining < 32 && !recent.empty())
        {
            break;
        }
        const std::size_t cost = messageCost(message);
        if (cost > remaining)
        {
            // The newest turn is the one being answered. If it alone does not fit,
            // it is compacted rather than dropped, because a request with no
            // current turn in it is not a smaller request -- it is a different one.
            if (recent.empty() &&
                remaining > ChatTemplateTokensPerMessage)
            {
                message["content"] = CompactCounted(
                    content, remaining - ChatTemplateTokensPerMessage, count,
                    CompactNewestMessage);
                used += messageCost(message);
                recent.push_back(std::move(message));
            }
            break;
        }
        used += cost;
        recent.push_back(std::move(message));
    }
    std::reverse(recent.begin(), recent.end());
    for (json& message : recent)
    {
        bounded.push_back(std::move(message));
    }
    return bounded;
}

} // namespace revia::llm
