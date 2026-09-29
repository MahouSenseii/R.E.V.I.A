#include "Skills/mcpRegistry.h"

#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::skills
{

namespace
{
using json = nlohmann::json;

// The argument names a schema declares, for the planner: "sceneName (string, required)".
std::string SummarizeSchema(const std::string& schemaJson)
{
    json schema;
    try
    {
        schema = json::parse(schemaJson);
    }
    catch (const std::exception&)
    {
        return {};
    }
    if (!schema.is_object() || !schema.contains("properties") || !schema["properties"].is_object())
    {
        return {};
    }
    std::vector<std::string> required;
    if (schema.contains("required") && schema["required"].is_array())
    {
        for (const json& name : schema["required"])
            if (name.is_string()) required.push_back(name.get<std::string>());
    }
    std::string summary;
    for (const auto& [name, property] : schema["properties"].items())
    {
        if (!summary.empty()) summary += ", ";
        summary += name;
        const std::string type = property.is_object() ? property.value("type", "") : "";
        const bool isRequired = std::find(required.begin(), required.end(), name) != required.end();
        if (!type.empty() || isRequired)
        {
            summary += " (" + type + (isRequired ? (type.empty() ? "required" : ", required") : "") + ")";
        }
    }
    return summary;
}
} // namespace

McpRegistry::McpRegistry()
    : factory([](const McpManifest& manifest)
      {
          return std::make_unique<McpClient>(manifest.url, manifest.headers);
      })
{
}

void McpRegistry::SetClientFactory(ClientFactory inputFactory)
{
    std::lock_guard lock(mutex);
    if (inputFactory) factory = std::move(inputFactory);
}

void McpRegistry::Load(const std::filesystem::path& manifestDirectory)
{
    std::lock_guard lock(mutex);
    servers.clear();
    loadErrors.clear();
    for (McpManifest& manifest : LoadManifestDirectory(manifestDirectory, loadErrors))
    {
        auto server = std::make_unique<Server>();
        server->manifest = std::move(manifest);
        servers.push_back(std::move(server));
    }
}

std::vector<std::string> McpRegistry::LoadErrors() const
{
    std::lock_guard lock(mutex);
    return loadErrors;
}

McpRegistry::Server* McpRegistry::Find(const std::string& serverId)
{
    for (auto& server : servers)
    {
        if (server->manifest.id == serverId) return server.get();
    }
    return nullptr;
}

bool McpRegistry::ConnectLocked(Server& server, std::string& outError)
{
    server.connected = false;
    server.listed = false;
    server.live.clear();
    server.client = factory(server.manifest);
    if (!server.client)
    {
        server.error = "No client could be made for " + server.manifest.url + ".";
        outError = server.error;
        return false;
    }
    if (!server.client->Connect(server.error))
    {
        outError = server.error;
        return false;
    }
    server.serverName = server.client->ServerName();
    server.protocolVersion = server.client->ProtocolVersion();
    std::string listError;
    for (McpToolInfo& tool : server.client->ListTools(listError))
    {
        server.live[tool.name] = std::move(tool);
    }
    if (!listError.empty())
    {
        server.error = listError;
        outError = listError;
        return false;
    }
    server.listed = true;
    server.connected = true;
    server.error.clear();
    return true;
}

bool McpRegistry::Connect(const std::string& serverId, std::string& outError)
{
    std::lock_guard lock(mutex);
    Server* server = Find(serverId);
    if (server == nullptr)
    {
        outError = "No MCP server named \"" + serverId + "\" has a manifest.";
        return false;
    }
    return ConnectLocked(*server, outError);
}

McpToolStatus McpRegistry::StatusOf(const Server& server, const McpToolPin& pin)
{
    McpToolStatus status;
    status.server = server.manifest.id;
    status.name = pin.name;
    status.risk = pin.risk;
    if (!server.manifest.enabled)
    {
        status.state = "server disabled";
        return status;
    }
    if (!pin.enabled)
    {
        status.state = "disabled";
        return status;
    }
    if (pin.digest.empty())
    {
        status.state = "unpinned";
        return status;
    }
    if (!server.listed)
    {
        status.state = server.error.empty() ? "not connected" : "not connected: " + server.error;
        return status;
    }
    const auto live = server.live.find(pin.name);
    if (live == server.live.end())
    {
        status.state = "absent from the server";
        return status;
    }
    if (ToolDigest(live->second.description, live->second.inputSchema) != pin.digest)
    {
        status.state = "changed since it was pinned; /skills pin " + server.manifest.id + " accepts it";
        return status;
    }
    status.offered = true;
    status.state = "offered";
    return status;
}

bool McpRegistry::Pin(const std::string& serverId, std::string& outError)
{
    std::lock_guard lock(mutex);
    Server* server = Find(serverId);
    if (server == nullptr)
    {
        outError = "No MCP server named \"" + serverId + "\" has a manifest.";
        return false;
    }
    if (!server->listed && !ConnectLocked(*server, outError)) return false;
    std::vector<McpToolPin> pinned;
    for (const auto& [name, tool] : server->live)
    {
        if (!ValidToolName(name)) continue;
        McpToolPin pin;
        pin.name = name;
        pin.description = tool.description;
        pin.inputSchema = tool.inputSchema;
        pin.digest = ToolDigest(tool.description, tool.inputSchema);
        if (const McpToolPin* previous = server->manifest.FindTool(name))
        {
            pin.risk = previous->risk;
            pin.enabled = previous->enabled;
        }
        pinned.push_back(std::move(pin));
    }
    server->manifest.tools = std::move(pinned);
    return SaveManifest(server->manifest, outError);
}

bool McpRegistry::SetEnabled(const std::string& serverId, const bool enabled, std::string& outError)
{
    std::lock_guard lock(mutex);
    Server* server = Find(serverId);
    if (server == nullptr)
    {
        outError = "No MCP server named \"" + serverId + "\" has a manifest.";
        return false;
    }
    server->manifest.enabled = enabled;
    return SaveManifest(server->manifest, outError);
}

std::optional<actions::RiskLevel> McpRegistry::ResolveTool(
    const std::string& serverId, const std::string& toolName, std::string& outReason)
{
    std::lock_guard lock(mutex);
    Server* server = Find(serverId);
    if (server == nullptr)
    {
        outReason = "No MCP server named \"" + serverId + "\" has a manifest.";
        return std::nullopt;
    }
    if (!server->manifest.enabled)
    {
        outReason = "The MCP server \"" + serverId + "\" is disabled in its manifest.";
        return std::nullopt;
    }
    const McpToolPin* pin = server->manifest.FindTool(toolName);
    if (pin == nullptr)
    {
        outReason = "The tool \"" + toolName + "\" is not in the manifest for \"" + serverId + "\".";
        return std::nullopt;
    }
    if (!server->listed)
    {
        std::string error;
        if (!ConnectLocked(*server, error))
        {
            outReason = "The MCP server \"" + serverId + "\" could not be reached: " + error;
            return std::nullopt;
        }
    }
    const McpToolStatus status = StatusOf(*server, *pin);
    if (!status.offered)
    {
        outReason = "The tool \"" + toolName + "\" on \"" + serverId + "\" is not offered: " + status.state + ".";
        return std::nullopt;
    }
    return pin->risk;
}

McpToolResult McpRegistry::Call(
    const std::string& serverId, const std::string& toolName, const std::string& argumentsJson)
{
    McpToolResult result;
    std::string reason;
    if (!ResolveTool(serverId, toolName, reason))
    {
        result.reason = reason;
        return result;
    }
    std::lock_guard lock(mutex);
    Server* server = Find(serverId);
    if (server == nullptr || !server->client)
    {
        result.reason = "The MCP server \"" + serverId + "\" is not connected.";
        return result;
    }
    return server->client->CallTool(toolName, argumentsJson);
}

std::string McpRegistry::PlannerCatalog()
{
    std::lock_guard lock(mutex);
    std::ostringstream catalog;
    for (auto& server : servers)
    {
        if (!server->manifest.enabled) continue;
        if (!server->listed)
        {
            std::string error;
            (void)ConnectLocked(*server, error);
        }
        for (const McpToolPin& pin : server->manifest.tools)
        {
            const McpToolStatus status = StatusOf(*server, pin);
            if (!status.offered) continue;
            catalog << "\n- server \"" << server->manifest.id << "\", tool \"" << pin.name << "\" ("
                    << actions::ToString(pin.risk) << "): " << pin.description;
            const std::string arguments = SummarizeSchema(pin.inputSchema);
            if (!arguments.empty()) catalog << " Arguments: " << arguments << ".";
        }
    }
    return catalog.str();
}

std::vector<McpServerStatus> McpRegistry::Status() const
{
    std::lock_guard lock(mutex);
    std::vector<McpServerStatus> statuses;
    for (const auto& server : servers)
    {
        McpServerStatus status;
        status.id = server->manifest.id;
        status.enabled = server->manifest.enabled;
        status.connected = server->connected;
        status.url = server->manifest.url;
        status.serverName = server->serverName;
        status.protocolVersion = server->protocolVersion;
        status.error = server->error;
        for (const McpToolPin& pin : server->manifest.tools)
        {
            status.tools.push_back(StatusOf(*server, pin));
        }
        statuses.push_back(std::move(status));
    }
    return statuses;
}

std::string McpRegistry::Describe() const
{
    std::ostringstream text;
    const std::vector<McpServerStatus> statuses = Status();
    if (statuses.empty())
    {
        text << "No MCP manifests are loaded.";
    }
    for (const McpServerStatus& server : statuses)
    {
        text << (text.str().empty() ? "" : "\n") << server.id << ": "
             << (server.enabled ? "enabled" : "disabled") << ", " << server.url
             << (server.connected ? ", connected" + (server.serverName.empty()
                    ? std::string{} : " to " + server.serverName) +
                    (server.protocolVersion.empty() ? std::string{} : " (" + server.protocolVersion + ")")
                : server.error.empty() ? std::string(", not connected") : ", not connected: " + server.error);
        if (server.tools.empty())
        {
            text << "\n  no tools pinned; /skills pin " << server.id << " imports the server's tools";
        }
        for (const McpToolStatus& tool : server.tools)
        {
            text << "\n  " << tool.name << " (" << actions::ToString(tool.risk) << "): " << tool.state;
        }
    }
    std::lock_guard lock(mutex);
    for (const std::string& error : loadErrors)
    {
        text << "\n  manifest not loaded: " << error;
    }
    return text.str();
}

bool McpRegistry::Empty() const
{
    std::lock_guard lock(mutex);
    return servers.empty();
}

} // namespace revia::skills
