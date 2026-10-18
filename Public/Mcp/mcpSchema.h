#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace revia::mcp
{

// A server's inputSchema, rewritten into the closed subset the chat tool registry
// validates strictly: objects with typed properties and no others; strings, integers,
// numbers and booleans with bounds; arrays of one item type with a maximum length;
// enums of scalars. Annotations (title, default, format, examples) are dropped.
// Anything that constrains values in a way the registry cannot check -- anyOf, $ref,
// pattern, open additionalProperties, a list of types -- refuses the whole tool, and so
// do nesting past four levels and a schema over MaximumSchemaBytes. Returns the schema
// as the registry stores it, or empty with `error` set.
inline constexpr std::size_t MaximumSchemaBytes = 16384;
[[nodiscard]] std::string NormalizeInputSchema(const nlohmann::json& schema, std::string& error);

// "mcp_<server>_<tool>", or empty when the server's tool name holds characters a
// function name may not, or the whole name would pass 64 characters.
[[nodiscard]] std::string ChatToolName(const std::string& serverId, const std::string& toolName);

} // namespace revia::mcp
