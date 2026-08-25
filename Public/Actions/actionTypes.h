#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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
    InvokeControl,
    WebSearch
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
    struct ElementResolutionEvidence
    {
        bool visionResolved = false;
        std::string modelTarget;
        int regionLeft = 0;
        int regionTop = 0;
        int regionRight = 0;
        int regionBottom = 0;
        double modelConfidence = 0.0;
        std::string resolvedName;
        std::string resolvedAutomationId;
        std::string resolvedRuntimeId;
        int resolvedControlType = 0;
        int boundsLeft = 0;
        int boundsTop = 0;
        int boundsRight = 0;
        int boundsBottom = 0;
        double spatialAgreement = 0.0;
        double nameAgreement = 0.0;
        double matchConfidence = 0.0;
    };

    std::string id;
    ActionType type = ActionType::Unknown;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string application;
    std::string windowTitle;
    std::string control;
    std::string value;
    // Present only after the vision-to-UIA resolver has produced a typed element
    // reference. Execution re-finds this exact runtime id and fails closed if it changed.
    ElementResolutionEvidence resolution;
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
    // Machine-readable executor provenance. Internet activity uses this to distinguish
    // the dedicated visible browser from an explicitly reported API fallback.
    std::string backend;
};

struct ActionOutcome
{
    PolicyDecision policy;
    ActionResult result;
};

struct CapabilitySettings
{
    struct InternetAccess
    {
        // Network access is opt-in. The model never receives a general socket or URL;
        // it may submit only a search query to this configured provider.
        bool enabled = false;
        bool automaticLookup = true;
        std::string provider = "duckduckgo";
        std::vector<std::string> approvedHosts = {
            "api.duckduckgo.com", "en.wikipedia.org"};
        int requestTimeoutMs = 8000;
        std::size_t maxResponseBytes = 256U * 1024U;
        int maxRequestsPerMinute = 12;
        int maxResults = 5;
        // When enabled, the same typed WebSearch action is fulfilled by a dedicated,
        // visible Edge/Chrome profile. The model still receives only bounded text and
        // source URLs; it never receives a socket, URL bar, selector, or script surface.
        bool visibleBrowser = false;
        // Separate authority: ordinary automatic lookup never silently grants Revia
        // permission to invent and research topics while no user turn is active.
        bool autonomousResearch = false;
        int visibleBrowserPort = 8095;
        int visibleBrowserStartupTimeoutMs = 8000;
        int visibleBrowserRequestTimeoutMs = 30000;
        int visibleBrowserMaxPages = 3;
        int visibleBrowserStepDelayMs = 250;
    };

    ExecutionMode mode = ExecutionMode::Supervised;
    std::vector<std::filesystem::path> approvedRoots;
    std::vector<std::string> approvedApplications;
    // Per executable, exact accessible names/automation ids or an explicit "*". Merely
    // approving an executable does not silently approve every mutable control it exposes.
    std::map<std::string, std::vector<std::string>> approvedControls;
    RiskLevel autoApproveRiskThrough = RiskLevel::ReadOnly;
    bool createMissingApprovedRoots = true;
    std::uintmax_t maxReadBytes = 1024U * 1024U;
    std::size_t maxDirectoryEntries = 500;
    std::size_t maxAffectedEntries = 200;
    int maxDesktopActionsPerMinute = 12;
    int minimumDesktopActionIntervalMs = 250;
    InternetAccess internet;
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
