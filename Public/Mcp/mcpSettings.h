#pragma once

#include <string>
#include <vector>

// One Model Context Protocol server the owner chose to connect. Nothing here is on by
// default: a server must be enabled, and each of its tools named in allowedTools, before
// the model can see it.
struct mcpServerSettings
{
    // Lowercase letters, digits, '-' and '_'; it names the server's tools as
    // mcp_<id>_<tool>, so it is short and unique.
    std::string id;
    bool bEnabled = false;
    // "stdio" starts `command` as a child process; "http" posts to `url`.
    std::string transport = "stdio";
    // Absolute path to the server program. Started directly, never through a shell.
    std::string command;
    std::vector<std::string> args;
    // Names of Revia's own environment variables copied to the server. Nothing else
    // from her environment reaches it.
    std::vector<std::string> envAllowlist;
    // https://..., or http://127.0.0.1:<port>/... for a server on this PC.
    std::string url;
    int timeoutSeconds = 30;
    // The server's own tool names that may be offered. Empty offers none.
    std::vector<std::string> allowedTools;
    // Ask before every call. Only the owner can turn this off, per server.
    bool bRequireConfirmation = true;
};

struct mcpSettings
{
    std::vector<mcpServerSettings> servers;
};

// The checks configManager applies to every server entry. Empty means valid.
[[nodiscard]] std::string ValidateMcpServerSettings(const mcpServerSettings& server);
