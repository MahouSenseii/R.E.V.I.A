#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace revia::policy
{

class CapabilityPolicy
{
public:
    explicit CapabilityPolicy(actions::CapabilitySettings settings);

    [[nodiscard]] actions::PolicyDecision Evaluate(
        const actions::ActionRequest& request) const;

    [[nodiscard]] const actions::CapabilitySettings& Settings() const;

    // Where an MCP tool's pinned risk comes from: the registry, through the runtime.
    // Without one every MCP tool is blocked, which is what a goal's scoped policy gets
    // and what the runtime gets before its registry is loaded.
    using McpToolResolver = std::function<std::optional<actions::RiskLevel>(
        const std::string& server, const std::string& tool, std::string& outReason)>;
    void SetMcpToolResolver(McpToolResolver resolver);

private:
    [[nodiscard]] std::filesystem::path ResolveForPolicy(
        const std::filesystem::path& value) const;
    [[nodiscard]] bool IsWithinApprovedRoot(
        const std::filesystem::path& lexicalPath,
        const std::filesystem::path& canonicalPath) const;
    [[nodiscard]] bool HasReparsePointBelowApprovedRoot(
        const std::filesystem::path& lexicalPath) const;
    [[nodiscard]] std::optional<std::filesystem::path> FindLexicalRoot(
        const std::filesystem::path& lexicalPath) const;

    actions::CapabilitySettings settings;
    std::vector<std::filesystem::path> lexicalRoots;
    std::vector<std::filesystem::path> canonicalRoots;
    McpToolResolver mcpToolResolver;
};

} // namespace revia::policy
