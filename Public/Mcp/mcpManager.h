#pragma once

#include "Agents/toolRegistry.h"
#include "Audit/mcpToolAudit.h"
#include "Mcp/mcpClient.h"
#include "Mcp/mcpSettings.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::mcp
{

// What the owner is asked before an MCP tool runs.
struct McpConfirmationRequest
{
    std::string server;
    std::string tool;
    // The validated arguments, bounded for display.
    std::string arguments;
};

// The boundary an MCP tool call passes through, supplied by the session for one turn.
struct McpGatePorts
{
    // Asks the owner. Empty when no prompt is available; a server that requires
    // confirmation then runs nothing.
    std::function<bool(const McpConfirmationRequest&)> confirm;
    // Writes one audit record. Required: when it is missing or fails, nothing is sent.
    std::function<bool(const audit::McpToolAuditEntry&)> audit;
};

struct McpServerStatus
{
    std::string id;
    bool enabled = false;
    std::string transport;
    // "off", "not started", "connected", "failed".
    std::string state;
    std::string detail;
    std::string serverName;
    bool requireConfirmation = true;
    // Chat names of the tools offered to the model.
    std::vector<std::string> offered;
    // Tools the server has that are not offered, each with why.
    std::vector<std::string> withheld;
};

// Owns the connections to the owner's configured MCP servers and turns their allowlisted
// tools into chat tools. A server is connected on first use, by a private turn or /mcp,
// never at startup, and one that fails is not retried for a minute so a broken server
// cannot slow every reply.
class McpManager
{
  public:
    using TransportFactory = std::function<std::unique_ptr<McpTransport>(const mcpServerSettings&)>;

    // The default factory starts stdio servers through the owned-process layer and
    // reaches http servers through WinHTTP.
    explicit McpManager(TransportFactory factory = {});
    ~McpManager();
    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;

    // Replaces the configured servers. A server whose settings changed is disconnected.
    void Configure(const mcpSettings& settings);
    [[nodiscard]] bool AnyEnabled() const;
    // The chat tools for one turn. Empty for anything but a private audience. Connects
    // enabled servers that are not connected yet, and re-reads a list that changed.
    [[nodiscard]] std::vector<agents::ChatTool> ChatTools(bool privateAudience, const McpGatePorts& ports, std::stop_token stopToken);
    // Disconnects every server and connects each enabled one again.
    void Reconnect(std::stop_token stopToken);
    [[nodiscard]] std::vector<McpServerStatus> Status() const;
    [[nodiscard]] std::string StatusText() const;
    void Shutdown();

  private:
    struct Server;
    void EnsureConnected(Server& server, std::stop_token stopToken, bool force);
    static agents::ChatTool BuildTool(const std::shared_ptr<Server>& server, const McpToolInfo& tool, const std::string& chatName,
        const std::string& schema, const McpGatePorts& ports);

    TransportFactory factory;
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Server>> servers;
};

} // namespace revia::mcp
