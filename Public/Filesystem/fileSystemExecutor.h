#pragma once

#include "Actions/IActionExecutor.h"

#include <cstddef>
#include <cstdint>

namespace revia::filesystem
{

class FileSystemExecutor final : public actions::IActionExecutor
{
public:
    FileSystemExecutor(
        std::uintmax_t maxReadBytes,
        std::size_t maxDirectoryEntries,
        std::size_t maxAffectedEntries);

    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) override;

private:
    [[nodiscard]] actions::ActionResult ListDirectory(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) const;
    [[nodiscard]] actions::ActionResult ReadTextFile(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) const;
    [[nodiscard]] actions::ActionResult CreateDirectory(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) const;
    [[nodiscard]] actions::ActionResult CopyFile(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) const;
    [[nodiscard]] actions::ActionResult MovePath(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) const;
    [[nodiscard]] actions::ActionResult MoveToRecycleBin(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) const;
    [[nodiscard]] bool IsWithinAffectedEntryLimit(
        const std::filesystem::path& value,
        std::size_t& outCount) const;

    std::uintmax_t maxReadBytes;
    std::size_t maxDirectoryEntries;
    std::size_t maxAffectedEntries;
};

} // namespace revia::filesystem
