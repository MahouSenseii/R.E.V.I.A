#include "Agents/replyFormat.h"
#include "Core/speechAttribution.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <set>
#include <vector>

namespace revia::agents
{
namespace
{
std::string RequestText(const std::string& input)
{
    std::string text = conversation::ReadSpeechAttribution(input).userAuthoredText;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        std::string open;
        std::string close;
        if (text[index] == '`')
        {
            auto end = text.find_first_not_of('`', index);
            if (end == std::string::npos)
                end = text.size();
            open = close = text.substr(index, end - index);
        }
        else if (text[index] == '"' || (text[index] == '\'' && (index == 0 || !std::isalnum(static_cast<unsigned char>(text[index - 1])))))
            open = close = text.substr(index, 1);
        else if (text.compare(index, 3, "“") == 0)
        {
            open = "“";
            close = "”";
        }
        else if (text.compare(index, 3, "‘") == 0)
        {
            open = "‘";
            close = "’";
        }
        if (!open.empty())
        {
            auto end = text.find(close, index + open.size());
            while (end != std::string::npos && end > 0 && text[end - 1] == '\\')
                end = text.find(close, end + close.size());
            end = end == std::string::npos ? text.size() : end + close.size();
            std::fill(text.begin() + index, text.begin() + end, ' ');
            index = end - 1;
        }
        else
            text[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
    }
    std::string compact;
    compact.reserve(text.size());
    for (const unsigned char character : text)
    {
        if (std::isspace(character))
        {
            const char normalized = character == '\r' || character == '\n' ? '\n' : ' ';
            if (compact.empty() || !std::isspace(static_cast<unsigned char>(compact.back())))
                compact += normalized;
            else if (normalized == '\n')
                compact.back() = '\n';
        }
        else
            compact += character;
    }
    return compact;
}

std::optional<bool> RequestPolarity(const std::string& text, std::size_t position)
{
    const auto boundary = position ? text.find_last_of(".!?;\r\n", position - 1) : std::string::npos;
    std::string prefix =
        text.substr(boundary == std::string::npos ? 0 : boundary + 1, position - (boundary == std::string::npos ? 0 : boundary + 1));
    const auto first = prefix.find_first_not_of(" \t");
    if (first == std::string::npos)
        return true;
    prefix = prefix.substr(first, prefix.find_last_not_of(" \t") - first + 1);
    static const std::regex negation(R"(\b(?:do not|don't|never)\b)");
    if (std::regex_search(prefix, negation))
        return false;
    if (const auto comma = prefix.find_last_of(','); comma != std::string::npos)
    {
        prefix = prefix.substr(comma + 1);
        const auto clauseStart = prefix.find_first_not_of(" \t");
        if (clauseStart == std::string::npos)
            return true;
        prefix = prefix.substr(clauseStart);
    }
    for (const std::string lead : {"please", "now", "then", "and", "so", "instead", "actually", "actually please", "can you", "could you",
             "would you", "can you please", "could you please", "would you please"})
        if (prefix == lead)
            return true;
    for (const std::string lead : {"do not", "don't", "never", "please do not", "please don't", "don't ever", "do not ever"})
        if (prefix == lead)
            return false;
    return std::nullopt;
}
}

ReplyFormat RequestedReplyFormat(const std::string& input)
{
    if (input.size() > MaximumJsonReplyBytes)
        return ReplyFormat::Conversation;
    const std::string text = RequestText(input);
    static const std::regex request(
        R"(\b(?:return|reply|respond|answer|output|produce|write|give|use)\s+(?:(?:the|your)\s+(?:answer|result|response)\s+)?(?:(?:only|a|an)\s+){0,2}(?:top[- ]level\s+)?(?:(?:in|as)\s+)?(?:only\s+)?(json|prose|plain text)(?:\s+(?:as\s+(?:an?\s+)?)?(?:top[- ]level\s+)?(object|array))?\b)");
    ReplyFormat format = ReplyFormat::Conversation;
    for (auto match = std::sregex_iterator(text.begin(), text.end(), request); match != std::sregex_iterator(); ++match)
    {
        const auto polarity = RequestPolarity(text, static_cast<std::size_t>(match->position()));
        if (!polarity)
            continue;
        if (!*polarity || (*match)[1] != "json")
            format = ReplyFormat::Conversation;
        else
            format = (*match)[2] == "array" ? ReplyFormat::JsonArray : ReplyFormat::JsonObject;
    }
    return format;
}

bool IsCompleteJsonReply(const std::string& text, std::size_t maxBytes)
{
    if (text.size() > std::min(maxBytes, MaximumJsonReplyBytes) || !utf8::IsValid(text))
        return false;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || (text[first] != '{' && text[first] != '['))
        return false;
    try
    {
        std::vector<std::set<std::string>> keys;
        const auto parsed = nlohmann::json::parse(text,
            [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
            {
                if (depth > 32)
                    throw std::runtime_error("JSON reply depth exceeded.");
                if (event == nlohmann::json::parse_event_t::object_start)
                    keys.emplace_back();
                else if (event == nlohmann::json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                    throw std::runtime_error("Duplicate JSON reply key.");
                else if (event == nlohmann::json::parse_event_t::object_end)
                    keys.pop_back();
                return true;
            });
        return parsed.is_object() || parsed.is_array();
    }
    catch (const std::exception&)
    {
        return false;
    }
}
}
