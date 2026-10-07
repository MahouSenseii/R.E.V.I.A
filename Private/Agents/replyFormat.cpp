#include "Agents/replyFormat.h"
#include "Core/speechAttribution.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <vector>

namespace revia::agents
{
namespace
{
std::string RequestText(const std::string& input, bool compactWhitespace = true)
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
    if (!compactWhitespace)
        return text;
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

struct DirectiveText
{
    std::string text;
    std::vector<std::size_t> offsets;
};

DirectiveText Directives(const std::string& input)
{
    const auto masked = RequestText(input, false);
    DirectiveText result;
    for (std::size_t index = 0; index < masked.size(); ++index)
    {
        const auto character = static_cast<unsigned char>(masked[index]);
        if (std::isspace(character) && !result.text.empty() && std::isspace(static_cast<unsigned char>(result.text.back())))
        {
            if (character == '\r' || character == '\n')
                result.text.back() = '\n';
            continue;
        }
        result.offsets.push_back(index);
        result.text += std::isspace(character) ? (character == '\r' || character == '\n' ? '\n' : ' ') : static_cast<char>(character);
    }
    result.offsets.push_back(masked.size());
    return result;
}

const std::regex& DirectivePattern()
{
    static const std::regex request(
        R"(\b(?:return|reply|respond|answer|output|produce|write|give|use)\s+(?:(?:the|your)\s+(?:answer|result|response)\s+)?(?:(?:only|a|an)\s+){0,2}(?:top[- ]level\s+)?(?:(?:in|as)\s+)?(?:only\s+)?(?:valid\s+)?(json|prose|plain text)(?:\s+(?:as\s+(?:an?\s+)?)?(?:top[- ]level\s+)?(object|array))?\b|\b((?:follow|use)\s+this\s+json\s+schema\s*:))");
    return request;
}

const std::string& DeliveryPattern()
{
    static const std::string body =
        R"((?:return|reply|respond|output)\s+(?:(?:only\s+)?(?:(?:in|as)\s+)?(?:valid\s+)?json(?:\s+(?:object|array))?|only\s+the\s+(?:valid\s+)?json\s+(?:object|array))(?:,\s*without\s+(?:prose|extra\s+commentary)(?:\s+or\s+(?:a\s+)?code\s+fences?)?)?)";
    return body;
}

std::string NormalizedClause(const std::string& clause)
{
    std::string normalized;
    for (const unsigned char character : clause)
    {
        if (std::isspace(character))
        {
            if (!normalized.empty() && normalized.back() != ' ')
                normalized += ' ';
        }
        else
            normalized += static_cast<char>(std::tolower(character));
    }
    if (!normalized.empty() && normalized.back() == ' ')
        normalized.pop_back();
    return normalized;
}

bool PlainDelivery(const std::string& clause)
{
    static const std::regex plain("^" + DeliveryPattern() + "[.!?;]?$");
    return std::regex_match(NormalizedClause(clause), plain);
}

std::string SchemaPrefix(const std::string& suffix)
{
    const auto begin = suffix.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos || suffix[begin] != '{')
        return {};
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = begin; index < suffix.size() && index - begin < MaximumReplySchemaBytes; ++index)
    {
        const char character = suffix[index];
        if (quoted)
        {
            if (!escaped && character == '"')
                quoted = false;
            escaped = !escaped && character == '\\';
        }
        else if (character == '"')
            quoted = true;
        else if (character == '{' || character == '[')
            ++depth;
        else if (character == '}' || character == ']')
        {
            if (--depth == 0)
                return suffix.substr(begin, index - begin + 1);
        }
    }
    return {};
}

struct DeclaredSchemaRoot
{
    ReplyFormat kind;
    std::size_t end;
};

std::optional<DeclaredSchemaRoot> ReadSchemaRoot(const std::string& input, std::size_t begin, bool directIntroduction)
{
    begin = input.find_first_not_of(" \t\r\n", begin);
    if (begin == std::string::npos)
        return std::nullopt;
    if (!directIntroduction)
    {
        constexpr std::string_view legacyLead = "using this schema:";
        if (NormalizedClause(input.substr(begin, legacyLead.size())) != legacyLead)
            return std::nullopt;
        begin = input.find_first_not_of(" \t\r\n", begin + legacyLead.size());
        if (begin == std::string::npos)
            return std::nullopt;
    }
    const auto schemaText = SchemaPrefix(input.substr(begin));
    if (!IsCompleteJsonReply(schemaText, MaximumReplySchemaBytes))
        return std::nullopt;
    try
    {
        const auto schema = nlohmann::json::parse(schemaText);
        if (!schema.contains("type"))
            return std::nullopt;
        if (schema["type"] == "object")
            return DeclaredSchemaRoot{ReplyFormat::JsonObject, begin + schemaText.size()};
        if (schema["type"] == "array")
            return DeclaredSchemaRoot{ReplyFormat::JsonArray, begin + schemaText.size()};
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    return std::nullopt;
}
}

ReplyFormat RequestedReplyFormat(const std::string& input)
{
    if (input.size() > MaximumJsonReplyBytes)
        return ReplyFormat::Conversation;
    const auto directives = Directives(input);
    const auto& text = directives.text;
    const auto& request = DirectivePattern();
    ReplyFormat format = ReplyFormat::Conversation;
    bool schemaRoot = false;
    std::size_t previousEnd = 0;
    for (auto match = std::sregex_iterator(text.begin(), text.end(), request); match != std::sregex_iterator(); ++match)
    {
        const auto polarity = RequestPolarity(text, static_cast<std::size_t>(match->position()));
        if (!polarity)
            continue;
        const auto matchEnd = directives.offsets[static_cast<std::size_t>(match->position() + match->length())];
        if (!*polarity || (!(*match)[3].matched && (*match)[1] != "json"))
        {
            format = ReplyFormat::Conversation;
            schemaRoot = false;
        }
        else if ((*match)[3].matched)
        {
            format = ReplyFormat::JsonObject;
            schemaRoot = false;
            if (const auto declared = ReadSchemaRoot(input, matchEnd, true))
            {
                format = declared->kind;
                schemaRoot = true;
                previousEnd = declared->end;
            }
        }
        else
        {
            const auto begin = directives.offsets[static_cast<std::size_t>(match->position())];
            const auto end = input.find_first_of(".!?;\r\n", begin);
            const auto clauseEnd = end == std::string::npos ? input.size() : end;
            const auto deliveryKind = (*match)[2] == "array" ? ReplyFormat::JsonArray : ReplyFormat::JsonObject;
            const bool compatible = schemaRoot && (!(*match)[2].matched || deliveryKind == format) && begin >= previousEnd &&
                                    input.substr(previousEnd, begin - previousEnd).find_first_not_of(" \t\r\n.!?;") == std::string::npos &&
                                    PlainDelivery(input.substr(begin, clauseEnd - begin));
            if (!compatible)
            {
                format = (*match)[2] == "array" ? ReplyFormat::JsonArray : ReplyFormat::JsonObject;
                schemaRoot = false;
                if (const auto declared = ReadSchemaRoot(input, matchEnd, false);
                    declared && (!(*match)[2].matched || format == declared->kind))
                {
                    format = declared->kind;
                    schemaRoot = true;
                    previousEnd = declared->end;
                }
            }
            else
                previousEnd = clauseEnd;
        }
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

namespace
{
using Json = nlohmann::json;

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool AdmittedSchema(const Json& schema, std::size_t depth, std::size_t& keyCount)
{
    if (!schema.is_object() || depth > MaximumReplySchemaDepth)
        return false;
    static const std::set<std::string> allowed = {
        "type", "properties", "required", "additionalProperties", "items", "minItems", "maxItems"};
    for (const auto& [key, value] : schema.items())
        if (!allowed.contains(key))
            return false;
    if (schema.empty())
        return true;
    if (!schema.contains("type") || !schema["type"].is_string())
        return false;
    const auto type = schema["type"].get<std::string>();
    if (type != "object" && type != "array" && type != "string" && type != "boolean" && type != "integer" && type != "number" &&
        type != "null")
        return false;
    for (const char* key : {"properties", "required", "additionalProperties"})
        if (schema.contains(key) && type != "object")
            return false;
    for (const char* key : {"items", "minItems", "maxItems"})
        if (schema.contains(key) && type != "array")
            return false;
    if (schema.contains("properties"))
    {
        if (!schema["properties"].is_object())
            return false;
        keyCount += schema["properties"].size();
        if (keyCount > MaximumReplySchemaKeys)
            return false;
        for (const auto& property : schema["properties"])
            if (!AdmittedSchema(property, depth + 1, keyCount))
                return false;
    }
    if (schema.contains("required"))
    {
        if (!schema["required"].is_array() || schema["required"].size() > MaximumReplySchemaKeys)
            return false;
        std::set<std::string> required;
        for (const auto& key : schema["required"])
            if (!key.is_string() || !required.insert(key.get<std::string>()).second || !schema.contains("properties") ||
                !schema["properties"].contains(key.get<std::string>()))
                return false;
    }
    if (schema.contains("additionalProperties") && !schema["additionalProperties"].is_boolean())
        return false;
    if (schema.contains("items") && !AdmittedSchema(schema["items"], depth + 1, keyCount))
        return false;
    for (const char* key : {"minItems", "maxItems"})
        if (schema.contains(key) &&
            (!schema[key].is_number_integer() || schema[key].get<double>() < 0 || schema[key].get<double>() > MaximumJsonReplyBytes))
            return false;
    if (schema.contains("minItems") && schema.contains("maxItems") && schema["minItems"].get<double>() > schema["maxItems"].get<double>())
        return false;
    return true;
}

bool SchemaMatches(const Json& value, const Json& schema)
{
    if (schema.empty())
        return true;
    const auto type = schema.at("type").get<std::string>();
    if ((type == "object" && !value.is_object()) || (type == "array" && !value.is_array()) || (type == "string" && !value.is_string()) ||
        (type == "boolean" && !value.is_boolean()) || (type == "integer" && !value.is_number_integer()) ||
        (type == "number" && !value.is_number()) || (type == "null" && !value.is_null()))
        return false;
    if (value.is_object())
    {
        if (schema.contains("required"))
            for (const auto& key : schema["required"])
                if (!value.contains(key.get<std::string>()))
                    return false;
        for (const auto& [key, field] : value.items())
        {
            if (schema.contains("properties") && schema["properties"].contains(key))
            {
                if (!SchemaMatches(field, schema["properties"][key]))
                    return false;
            }
            else if (schema.contains("additionalProperties") && !schema["additionalProperties"].get<bool>())
                return false;
        }
    }
    if (value.is_array())
    {
        if ((schema.contains("minItems") && value.size() < schema["minItems"].get<std::size_t>()) ||
            (schema.contains("maxItems") && value.size() > schema["maxItems"].get<std::size_t>()))
            return false;
        if (schema.contains("items"))
            for (const auto& item : value)
                if (!SchemaMatches(item, schema["items"]))
                    return false;
    }
    return true;
}

std::optional<Json> KeySchema(std::string text)
{
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    text = Trim(text);
    Json schema = {{"type", "object"}, {"properties", Json::object()}, {"required", Json::array()}, {"additionalProperties", false}};
    std::size_t position = 0;
    bool expectKey = true;
    while (position < text.size())
    {
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
            ++position;
        if (position == text.size())
            break;
        if (!expectKey)
        {
            if (text[position] == ',')
            {
                ++position;
                while (position < text.size() && text[position] == ' ')
                    ++position;
                if (text.compare(position, 4, "and ") == 0)
                    position += 4;
            }
            else if (text.compare(position, 4, "and ") == 0)
                position += 4;
            else
                return std::nullopt;
            expectKey = true;
            continue;
        }
        std::string key;
        if (text[position] == '"')
        {
            const auto begin = position++;
            bool escaped = false;
            for (; position < text.size(); ++position)
            {
                const char character = text[position];
                if (!escaped && character == '"')
                    break;
                escaped = !escaped && character == '\\';
            }
            if (position == text.size())
                return std::nullopt;
            key = Json::parse(text.substr(begin, ++position - begin)).get<std::string>();
        }
        else
        {
            const auto begin = position;
            if (!std::isalpha(static_cast<unsigned char>(text[position])) && text[position] != '_')
                return std::nullopt;
            while (position < text.size() && (std::isalnum(static_cast<unsigned char>(text[position])) || text[position] == '_'))
                ++position;
            key = text.substr(begin, position - begin);
        }
        if (schema["properties"].contains(key) || schema["properties"].size() >= MaximumReplySchemaKeys)
            return std::nullopt;
        schema["properties"][key] = Json::object();
        schema["required"].push_back(key);
        expectKey = false;
    }
    return expectKey ? std::nullopt : std::optional<Json>(std::move(schema));
}
}

static ReplyContract ExtractReplyContract(const std::string& input, bool retainDelivery, int remainingReaffirmations = 16)
{
    ReplyContract contract;
    contract.rootKind = RequestedReplyFormat(input);
    if (contract.rootKind == ReplyFormat::Conversation)
        return contract;
    contract.extractionStatus = ReplyContractStatus::TypeOnly;
    contract.schemaJson = contract.rootKind == ReplyFormat::JsonArray ? R"({"type":"array"})" : R"({"type":"object"})";
    // Only the admitted directive opens this clause. Preserve its quoted field names;
    // the speech/quote-masked search view still decides whether the directive exists.
    const auto& original = input;
    const auto directives = Directives(input);
    const auto& text = directives.text;
    const auto& offsets = directives.offsets;
    if (retainDelivery)
    {
        static const std::regex terminal("(^|[.!?;\\n])\\s*(" + DeliveryPattern() + ")[.!?;]?\\s*$");
        std::size_t inputEnd = original.size();
        std::size_t textEnd = text.size();
        for (int count = 0; count < 16; ++count)
        {
            std::smatch delivery;
            const auto view = text.substr(0, textEnd);
            if (!std::regex_search(view, delivery, terminal))
                break;
            const auto begin = offsets[static_cast<std::size_t>(delivery.position(2))];
            const auto rawClause = Trim(original.substr(begin, inputEnd - begin));
            const auto normalized = NormalizedClause(rawClause);
            // The search view masks quoted payloads. Require the entire original
            // clause to be plain delivery text before discarding any suffix.
            if (!PlainDelivery(rawClause))
                break;
            const auto deliveryKind = normalized.find("json array") != std::string::npos    ? ReplyFormat::JsonArray
                                      : normalized.find("json object") != std::string::npos ? ReplyFormat::JsonObject
                                                                                            : contract.rootKind;
            if (deliveryKind != contract.rootKind)
                break;
            inputEnd = begin;
            textEnd = static_cast<std::size_t>(delivery.position(2));
        }
        if (inputEnd < original.size())
        {
            const auto preceding = ExtractReplyContract(Trim(original.substr(0, inputEnd)), false);
            if (preceding.rootKind == contract.rootKind && (preceding.extractionStatus == ReplyContractStatus::ExplicitShape ||
                                                               preceding.extractionStatus == ReplyContractStatus::Unsupported))
                return preceding;
        }
    }
    const auto& request = DirectivePattern();
    std::size_t end = std::string::npos;
    std::size_t directiveBegin = 0;
    bool introducedSchema = false;
    for (auto match = std::sregex_iterator(text.begin(), text.end(), request); match != std::sregex_iterator(); ++match)
        if (RequestPolarity(text, static_cast<std::size_t>(match->position())).value_or(false))
        {
            directiveBegin = offsets[static_cast<std::size_t>(match->position())];
            end = offsets[static_cast<std::size_t>(match->position() + match->length())];
            introducedSchema = (*match)[3].matched;
        }
    if (end == std::string::npos)
        return contract;
    const auto suffix = Trim(original.substr(end));
    std::string lowerSuffix = suffix;
    std::transform(lowerSuffix.begin(), lowerSuffix.end(), lowerSuffix.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    static const std::regex keyLead(R"(^with\s+(?:exactly\s+)?keys?\s+)");
    std::smatch match;
    const bool keyRequest = std::regex_search(lowerSuffix, match, keyLead);
    const bool schemaRequest = introducedSchema || lowerSuffix.starts_with("using this schema:");
    if (!keyRequest && !schemaRequest)
        return contract;
    const auto unsupported = [&](const std::string& reason)
    {
        contract.extractionStatus = ReplyContractStatus::Unsupported;
        contract.reason = reason;
        return contract;
    };
    if (!utf8::IsValid(original))
        return unsupported("Explicit reply shape contains malformed UTF-8.");
    try
    {
        std::optional<Json> schema;
        if (keyRequest)
        {
            if (contract.rootKind != ReplyFormat::JsonObject || suffix.size() > MaximumReplySchemaBytes)
                return unsupported("Explicit key list exceeds its bounds or conflicts with the requested container.");
            schema = KeySchema(suffix.substr(match.length()));
        }
        else
        {
            auto schemaText = introducedSchema ? suffix : Trim(suffix.substr(std::string("using this schema:").size()));
            if (!schemaText.empty() && schemaText.back() == '.')
                schemaText.pop_back();
            if (schemaText.size() > MaximumReplySchemaBytes || !IsCompleteJsonReply(schemaText, MaximumReplySchemaBytes))
                return unsupported("Explicit schema is malformed, duplicated or exceeds its byte/depth limit.");
            schema = Json::parse(schemaText);
        }
        std::size_t keyCount = 0;
        const std::string rootType = contract.rootKind == ReplyFormat::JsonArray ? "array" : "object";
        if (!schema || !AdmittedSchema(*schema, 1, keyCount) || schema->value("type", std::string{}) != rootType ||
            schema->dump().size() > MaximumReplySchemaBytes)
            return unsupported("Explicit reply shape is ambiguous, unsupported or exceeds its key/depth limit.");
        contract.schemaJson = schema->dump();
        contract.extractionStatus = ReplyContractStatus::ExplicitShape;
        if (keyRequest && remainingReaffirmations > 0 && directiveBegin > 0)
        {
            const auto preceding = ExtractReplyContract(Trim(original.substr(0, directiveBegin)), false, remainingReaffirmations - 1);
            if (preceding.rootKind == contract.rootKind && preceding.extractionStatus == ReplyContractStatus::ExplicitShape)
            {
                const auto previousSchema = Json::parse(preceding.schemaJson);
                if (previousSchema.contains("properties") && previousSchema.contains("required") &&
                    previousSchema.contains("additionalProperties") && previousSchema["additionalProperties"] == false &&
                    previousSchema["properties"].size() == schema->at("properties").size() &&
                    previousSchema["required"].size() == schema->at("required").size())
                {
                    bool identicalKeys = true;
                    for (const auto& key : schema->at("required"))
                        identicalKeys = identicalKeys && previousSchema["properties"].contains(key.get<std::string>()) &&
                                        std::find(previousSchema["required"].begin(), previousSchema["required"].end(), key) !=
                                            previousSchema["required"].end();
                    if (identicalKeys)
                        return preceding;
                }
            }
        }
    }
    catch (const std::exception&)
    {
        return unsupported("Explicit reply shape could not be parsed without inference.");
    }
    return contract;
}

ReplyContract RequestedReplyContract(const std::string& input)
{
    return ExtractReplyContract(input, true);
}

bool MatchesReplyContract(const std::string& text, const ReplyContract& contract)
{
    if (contract.rootKind == ReplyFormat::Conversation)
        return true;
    if (!IsCompleteJsonReply(text))
        return false;
    try
    {
        const auto value = Json::parse(text);
        if (contract.rootKind == ReplyFormat::JsonArray ? !value.is_array() : !value.is_object())
            return false;
        const auto schema = Json::parse(contract.schemaJson);
        std::size_t keyCount = 0;
        return AdmittedSchema(schema, 1, keyCount) && SchemaMatches(value, schema);
    }
    catch (const std::exception&)
    {
        return false;
    }
}
}
