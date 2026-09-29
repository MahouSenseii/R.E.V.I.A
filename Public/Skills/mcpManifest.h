#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace revia::skills
{

// What an MCP server is allowed to be, written down by the owner.
//
// A tool description is prompt text a server sends: whatever it says, the model reads.
// A server that changes a tool's description after it was trusted -- the "rug pull" --
// changes what the model is told without anyone agreeing to it. So a manifest pins
// each tool: the description and input schema the owner saw, as a digest, and the risk
// they assigned. At every connection the live list is compared; a tool whose digest
// differs, or that is not in the manifest at all, is not offered. The model only ever
// sees the pinned text. Nothing here launches a process: a manifest names an HTTP
// endpoint the owner started, and /skills pin is the one step that trusts a server.
struct McpToolPin
{
    std::string name;
    // The pinned text, shown to the planner as the tool's description.
    std::string description;
    // The pinned input schema, as JSON text; shown to the planner in summary.
    std::string inputSchema;
    // Sha256Hex(description + "\n" + inputSchema) at the time of pinning.
    std::string digest;
    // What using it costs, decided by the owner: read_only, reversible_write or
    // destructive (the default for anything imported, which always asks).
    actions::RiskLevel risk = actions::RiskLevel::Destructive;
    bool enabled = true;
};

struct McpManifest
{
    std::string id;
    bool enabled = false;
    // "http": a Streamable HTTP endpoint. A stdio server reaches Revia through
    // Tools/Mcp/bridge.mjs, which the owner runs; Revia never spawns one.
    std::string transportKind = "http";
    std::string url;
    // Sent with every request: an owner's bearer token for a remote server.
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<McpToolPin> tools;
    std::filesystem::path path;

    [[nodiscard]] const McpToolPin* FindTool(const std::string& name) const;
};

[[nodiscard]] std::string ToolDigest(const std::string& description, const std::string& inputSchema);
[[nodiscard]] bool ValidToolName(const std::string& name);
[[nodiscard]] bool ValidServerId(const std::string& id);

[[nodiscard]] bool LoadManifest(const std::filesystem::path& path, McpManifest& outManifest, std::string& outError);
[[nodiscard]] bool SaveManifest(const McpManifest& manifest, std::string& outError);
// Every *.json in the directory that loads; the errors of those that do not.
[[nodiscard]] std::vector<McpManifest> LoadManifestDirectory(
    const std::filesystem::path& directory, std::vector<std::string>& outErrors);

} // namespace revia::skills
