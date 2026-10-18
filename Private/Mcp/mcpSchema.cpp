#include "Mcp/mcpSchema.h"

#include "Agents/toolRegistry.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace revia::mcp
{
namespace
{
using Json = nlohmann::json;

constexpr int MaximumDepth = 4;
constexpr std::size_t MaximumProperties = 64;
constexpr std::size_t MaximumEnumValues = 64;
constexpr std::size_t DefaultStringLength = 4096;
constexpr std::size_t DefaultArrayItems = 32;
constexpr std::size_t MaximumArrayItems = 64;
constexpr std::size_t MaximumDescriptionBytes = 300;

// Words that only describe a value. Dropping them changes nothing the registry checks.
const std::set<std::string>& Annotations()
{
    static const std::set<std::string> words = {"title", "default", "examples", "format", "$comment", "readOnly", "writeOnly", "deprecated"};
    return words;
}

std::string Described(const Json& node)
{
    if (!node.contains("description") || !node["description"].is_string())
        return {};
    std::string text = utf8::Sanitize(node["description"].get<std::string>());
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text.size() > MaximumDescriptionBytes ? utf8::Prefix(text, MaximumDescriptionBytes) : text;
}

bool Count(const Json& node, const char* key, const std::size_t ceiling, std::size_t& value, std::string& error)
{
    if (!node.contains(key))
        return true;
    if (!node[key].is_number_unsigned() && !(node[key].is_number_integer() && node[key].get<long long>() >= 0))
    {
        error = std::string("\"") + key + "\" must be a whole number.";
        return false;
    }
    value = std::min<std::size_t>(node[key].get<std::size_t>(), ceiling);
    return true;
}

bool Normalize(const Json& node, const int depth, Json& out, std::string& error)
{
    if (!node.is_object())
    {
        error = "A schema entry is not an object.";
        return false;
    }
    if (!node.contains("type") || !node["type"].is_string())
    {
        error = "Every schema entry needs one \"type\".";
        return false;
    }
    const std::string type = node["type"].get<std::string>();
    std::set<std::string> allowed = {"type", "description"};
    if (type == "object")
        allowed.insert({"properties", "required", "additionalProperties"});
    else if (type == "string")
        allowed.insert({"minLength", "maxLength", "enum", "const"});
    else if (type == "integer" || type == "number")
        allowed.insert({"minimum", "maximum", "enum", "const"});
    else if (type == "boolean")
        allowed.insert({"enum", "const"});
    else if (type == "array")
        allowed.insert({"items", "minItems", "maxItems"});
    else
    {
        error = "The type \"" + utf8::Prefix(utf8::Sanitize(type), 32) + "\" cannot be checked.";
        return false;
    }
    for (const auto& item : node.items())
    {
        if (!allowed.contains(item.key()) && !Annotations().contains(item.key()) && !(depth == 1 && item.key() == "$schema"))
        {
            error = "The keyword \"" + utf8::Prefix(utf8::Sanitize(item.key()), 32) + "\" cannot be checked strictly.";
            return false;
        }
    }

    out = Json::object();
    out["type"] = type;
    if (const std::string description = Described(node); !description.empty())
        out["description"] = description;

    if (node.contains("enum") || node.contains("const"))
    {
        const Json values = node.contains("const") ? Json::array({node["const"]}) : node["enum"];
        if (!values.is_array() || values.empty() || values.size() > MaximumEnumValues ||
            !std::all_of(values.begin(), values.end(), [](const Json& value) { return value.is_primitive() && !value.is_null(); }))
        {
            error = "An enum must list 1 to 64 plain values.";
            return false;
        }
        for (const auto& value : values)
        {
            const bool matches = (type == "string" && value.is_string() && utf8::IsValid(value.get<std::string>())) ||
                                 (type == "integer" && value.is_number_integer()) || (type == "number" && value.is_number()) ||
                                 (type == "boolean" && value.is_boolean());
            if (!matches)
            {
                error = "An enum value does not match its type.";
                return false;
            }
        }
        out["enum"] = values;
    }

    if (type == "object")
    {
        if (depth > MaximumDepth)
        {
            error = "The schema is nested more than four levels deep.";
            return false;
        }
        if (node.contains("additionalProperties") && node["additionalProperties"] != false)
        {
            error = "The schema accepts arguments it does not name.";
            return false;
        }
        const Json properties = node.value("properties", Json::object());
        if (!properties.is_object() || properties.size() > MaximumProperties)
        {
            error = "An object must name at most 64 properties.";
            return false;
        }
        out["properties"] = Json::object();
        for (const auto& property : properties.items())
        {
            if (property.key().empty() || property.key().size() > 64 || !utf8::IsValid(property.key()))
            {
                error = "A property name is empty, too long or not valid text.";
                return false;
            }
            Json normalized;
            if (!Normalize(property.value(), depth + 1, normalized, error))
                return false;
            out["properties"][property.key()] = std::move(normalized);
        }
        Json required = Json::array();
        if (node.contains("required"))
        {
            if (!node["required"].is_array())
            {
                error = "\"required\" must be a list of property names.";
                return false;
            }
            for (const auto& name : node["required"])
            {
                // A closed object can never hold a required key it does not describe.
                if (!name.is_string() || !properties.contains(name.get<std::string>()))
                {
                    error = "A required argument is not among the properties.";
                    return false;
                }
                required.push_back(name);
            }
        }
        out["required"] = std::move(required);
        out["additionalProperties"] = false;
        return true;
    }
    if (type == "string")
    {
        std::size_t minimum = 0;
        std::size_t maximum = DefaultStringLength;
        if (!Count(node, "minLength", agents::MaximumToolArgumentBytes, minimum, error) ||
            !Count(node, "maxLength", agents::MaximumToolArgumentBytes, maximum, error))
            return false;
        if (minimum > maximum)
        {
            error = "A string's minimum length is above its maximum.";
            return false;
        }
        out["minLength"] = minimum;
        out["maxLength"] = maximum;
        return true;
    }
    if (type == "integer" || type == "number")
    {
        for (const char* key : {"minimum", "maximum"})
        {
            if (!node.contains(key))
                continue;
            const Json& bound = node[key];
            const bool valid = type == "integer" ? bound.is_number_integer() : bound.is_number();
            if (!valid)
            {
                error = std::string("\"") + key + "\" does not match its type.";
                return false;
            }
            out[key] = bound;
        }
        return true;
    }
    if (type == "array")
    {
        if (depth >= MaximumDepth)
        {
            error = "The schema is nested more than four levels deep.";
            return false;
        }
        if (!node.contains("items") || !node["items"].is_object())
        {
            error = "An array must describe its items with one schema.";
            return false;
        }
        std::size_t minimum = 0;
        std::size_t maximum = DefaultArrayItems;
        if (!Count(node, "minItems", MaximumArrayItems, minimum, error) || !Count(node, "maxItems", MaximumArrayItems, maximum, error))
            return false;
        if (minimum > maximum)
        {
            error = "An array's minimum length is above its maximum.";
            return false;
        }
        Json items;
        if (!Normalize(node["items"], depth + 1, items, error))
            return false;
        out["items"] = std::move(items);
        out["minItems"] = minimum;
        out["maxItems"] = maximum;
        return true;
    }
    return true;
}
} // namespace

std::string NormalizeInputSchema(const Json& schema, std::string& error)
{
    error.clear();
    if (!schema.is_object() || schema.value("type", Json()) != "object")
    {
        error = "The input schema is not an object schema.";
        return {};
    }
    Json normalized;
    if (!Normalize(schema, 1, normalized, error))
        return {};
    std::string text = normalized.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (text.size() > MaximumSchemaBytes)
    {
        error = "The input schema is larger than Revia accepts.";
        return {};
    }
    return text;
}

std::string ChatToolName(const std::string& serverId, const std::string& toolName)
{
    if (toolName.empty())
        return {};
    std::string name = "mcp_" + serverId + "_";
    for (const unsigned char c : toolName)
    {
        if (std::isalnum(c) || c == '_' || c == '-')
            name.push_back(static_cast<char>(c));
        else if (c == '.')
            name.push_back('_');
        else
            return {};
    }
    return name.size() <= 64 ? name : std::string{};
}

} // namespace revia::mcp
