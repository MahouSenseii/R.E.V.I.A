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

    // The camera is the most physically invasive thing this application can reach, so it
    // is off until explicitly asked for and it lives behind the same capability file as
    // everything else rather than behind a comfort preference.
    //
    // Observation is not authority. Being allowed to take a frame grants nothing else:
    // anything Revia does because of what a frame contained still goes through the
    // ordinary typed action, policy, confirmation, and audit path.
    struct CameraAccess
    {
        bool enabled = false;
        // Which device, by the stable symbolic link. Empty means the first attached
        // camera, which is the right default on a laptop and the wrong one on a desk
        // with a capture card, so the setting exists.
        std::string preferredDevice;
        // A frame taken because Revia decided to look, rather than because the user
        // asked her to. Separate authority for the same reason autonomous research is
        // separate from ordinary lookup: consenting to answer "what am I holding?" is
        // not consenting to be watched.
        bool autonomousCapture = false;
        // Frames discarded while auto-exposure and auto-white-balance settle.
        //
        // Measured rather than guessed: on a USB 2.0 webcam the first frame is visibly
        // noisier than the tenth, and most of a capture's ~1.3s cost is opening the
        // device, so each extra frame is around 27ms. Ten buys a settled image for
        // roughly 200ms, which is a better trade than a fast picture of nothing.
        int warmupFrames = 10;
        // A floor between captures. Without one, a loop that captures per turn becomes
        // a recording with extra steps.
        int minimumIntervalMs = 4000;
        int maxCapturesPerMinute = 6;
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
    CameraAccess camera;
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
