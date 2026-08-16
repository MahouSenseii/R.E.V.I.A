#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::actions
{

enum class ActionType
{
    Unknown,
    ListDirectory,
    ReadTextFile,
    CreateDirectory,
    CopyFile,
    MoveFile,
    RenamePath,
    MoveToRecycleBin,
    InspectWindow,
    FocusWindow,
    SetControlText,
    InvokeControl
};

enum class RiskLevel
{
    ReadOnly = 0,
    ReversibleWrite = 1,
    Destructive = 2
};

enum class PolicyVerdict
{
    Allowed,
    RequiresConfirmation,
    Blocked
};

enum class ExecutionMode
{
    Disabled,
    Supervised,
    ApprovedScope
};

struct ActionRequest
{
    std::string id;
    ActionType type = ActionType::Unknown;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string application;
    std::string windowTitle;
    std::string control;
    std::string value;
    bool dryRun = false;
    std::string requestedBy = "user";
};

struct PolicyDecision
{
    PolicyVerdict verdict = PolicyVerdict::Blocked;
    RiskLevel risk = RiskLevel::Destructive;
    std::string reason;
    std::filesystem::path canonicalSource;
    std::filesystem::path canonicalDestination;
};

struct ActionResult
{
    bool attempted = false;
    bool succeeded = false;
    bool dryRun = false;
    std::string message;
    std::string content;
    std::vector<std::string> entries;
};

struct ActionOutcome
{
    PolicyDecision policy;
    ActionResult result;
};

struct CapabilitySettings
{
    ExecutionMode mode = ExecutionMode::Supervised;
    std::vector<std::filesystem::path> approvedRoots;
    std::vector<std::string> approvedApplications;
    RiskLevel autoApproveRiskThrough = RiskLevel::ReadOnly;
    bool createMissingApprovedRoots = true;
    std::uintmax_t maxReadBytes = 1024U * 1024U;
    std::size_t maxDirectoryEntries = 500;
    std::size_t maxAffectedEntries = 200;
};

[[nodiscard]] std::string ToString(ActionType value);
[[nodiscard]] std::string ToString(RiskLevel value);
[[nodiscard]] std::string ToString(PolicyVerdict value);
[[nodiscard]] std::string ToString(ExecutionMode value);
[[nodiscard]] ActionType ActionTypeFromString(const std::string& value);
[[nodiscard]] RiskLevel RiskLevelFromString(const std::string& value);
[[nodiscard]] ExecutionMode ExecutionModeFromString(const std::string& value);
[[nodiscard]] RiskLevel RiskForAction(ActionType value);
[[nodiscard]] std::string NewActionId();
[[nodiscard]] std::filesystem::path Utf8ToPath(const std::string& value);
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& value);

} // namespace revia::actions
