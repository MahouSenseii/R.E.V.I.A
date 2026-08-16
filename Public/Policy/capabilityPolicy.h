#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
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
};

} // namespace revia::policy
