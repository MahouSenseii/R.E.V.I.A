#pragma once

#include "Actions/actionTypes.h"
#include "Skills/mcpClient.h"
#include "Skills/mcpManifest.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::skills
{

// The MCP servers the owner wrote manifests for, and what each may do right now.
//
// A server is connected when first needed or on /skills connect, never launched. On
// connection its live tool list is checked against the manifest: a tool is offered
// only when it is in the manifest, enabled, and its description and schema still
// digest to what was pinned. The planner sees the pinned descriptions; the policy asks
// here for a tool's risk; the executor calls through here. Pinning -- accepting a
// server's tools as they are now, every new one at destructive risk -- is the one
// owner action that widens anything, and it rewrites the manifest on disk.
struct McpToolStatus
{
    std::string server;
    std::string name;
    actions::RiskLevel risk = actions::RiskLevel::Destructive;
    bool offered = false;
    // Why it is not offered, in a few words: disabled, unpinned, changed, absent.
    std::string state;
};

struct McpServerStatus
{
    std::string id;
    bool enabled = false;
    bool connected = false;
    std::string url;
    std::string serverName;
    std::string protocolVersion;
    std::string error;
    std::vector<McpToolStatus> tools;
};

class McpRegistry
{
public:
    // How a client is made for a manifest: a test hands in one aimed at a fake.
    using ClientFactory = std::function<std::unique_ptr<McpClient>(const McpManifest&)>;

    McpRegistry();

    void SetClientFactory(ClientFactory factory);
    // Loads every manifest in the directory. Nothing connects yet.
    void Load(const std::filesystem::path& manifestDirectory);
    [[nodiscard]] std::vector<std::string> LoadErrors() const;

    // Connects (or reconnects) one server and verifies its tools against the manifest.
    [[nodiscard]] bool Connect(const std::string& serverId, std::string& outError);
    // Imports the server's current tools as the pinned ones -- keeping the risk and
    // enabled flag of any tool already pinned, giving new ones destructive risk -- and
    // writes the manifest. The owner's explicit act of trust.
    [[nodiscard]] bool Pin(const std::string& serverId, std::string& outError);
    [[nodiscard]] bool SetEnabled(const std::string& serverId, bool enabled, std::string& outError);

    // The risk of a tool that may be called now, or nothing with the reason. Connects
    // the server on first use.
    [[nodiscard]] std::optional<actions::RiskLevel> ResolveTool(
        const std::string& serverId, const std::string& toolName, std::string& outReason);
    [[nodiscard]] McpToolResult Call(
        const std::string& serverId, const std::string& toolName, const std::string& argumentsJson);

    // The tools the planner may name, with their pinned descriptions and the argument
    // names from their pinned schemas. Empty when nothing is offered.
    [[nodiscard]] std::string PlannerCatalog();
    [[nodiscard]] std::vector<McpServerStatus> Status() const;
    [[nodiscard]] std::string Describe() const;
    [[nodiscard]] bool Empty() const;

private:
    struct Server
    {
        McpManifest manifest;
        std::unique_ptr<McpClient> client;
        bool connected = false;
        std::string error;
        std::string serverName;
        std::string protocolVersion;
        // The live tools as last listed, by name.
        std::map<std::string, McpToolInfo> live;
        bool listed = false;
    };
    // Caller holds the mutex.
    Server* Find(const std::string& serverId);
    bool ConnectLocked(Server& server, std::string& outError);
    [[nodiscard]] static McpToolStatus StatusOf(const Server& server, const McpToolPin& pin);

    mutable std::mutex mutex;
    ClientFactory factory;
    std::vector<std::unique_ptr<Server>> servers;
    std::vector<std::string> loadErrors;
};

} // namespace revia::skills
