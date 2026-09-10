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
    // Desktop operation. These reach the machine through synthesized input or a new
    // process rather than through a UI Automation pattern, so they are separately
    // permitted, separately rate limited, and never usable outside an approved window.
    LaunchApplication,
    MoveCursor,
    ClickPointer,
    DragPointer,
    ScrollPointer,
    PressKeys,
    TypeText,
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
    ApprovedScope,
    // The owner has delegated broad operation of this machine. It raises the automatic
    // approval ceiling to reversible work; it does not widen approved roots, approved
    // applications, approved controls, or any capability switch, and destructive work
    // still stops for confirmation. Environment is not authority: this is chosen, never
    // inferred from running in a virtual machine.
    OwnerFullAccess
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

    // Pointer and keyboard payload. Present only on the desktop-operation actions;
    // every other action type ignores it.
    struct DesktopInput
    {
        enum class PointerButton
        {
            Left,
            Right,
            Middle
        };

        // Virtual-desktop pixel coordinates, which on a multi-monitor desk can be
        // negative. Meaningful only when hasPoint is set: a vision-resolved click
        // re-finds its element and uses that element's current bounds instead, because
        // a coordinate captured before confirmation may no longer point at the target.
        int x = 0;
        int y = 0;
        bool hasPoint = false;
        // Where a drag ends. The button is held from the first point to this one and is
        // always released, including when the stop guard interrupts the movement.
        int endX = 0;
        int endY = 0;
        bool hasEndPoint = false;
        PointerButton button = PointerButton::Left;
        int clickCount = 1;
        // Wheel detents. Positive scrolls away from the user, or right when horizontal.
        int scrollClicks = 0;
        bool horizontalScroll = false;
        // A single normalized chord such as "ctrl+shift+s": modifiers held for exactly
        // one non-modifier key. It is deliberately not a macro language.
        std::string keys;
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
    DesktopInput input;
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
    // Execution may already have succeeded when recording its result fails.
    // Keep that fact in result; callers must stop dependent work on auditError.
    std::string auditError;

    [[nodiscard]] bool Succeeded() const { return result.succeeded && auditError.empty(); }
    [[nodiscard]] std::string Message() const;
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

    // Hands, as opposed to eyes. UI Automation asks an application to do something it
    // already exposes; synthesized input is indistinguishable from the person at the
    // keyboard, so it is off until the owner turns it on and stays confined to windows
    // that belong to an approved application.
    struct DesktopControl
    {
        // Where the hands may reach.
        //
        // ApprovedApplications is the narrow original: every action names an approved
        // executable, and input is confined to that executable's window. It is safe
        // because it is small, and small is also why it cannot learn anything general.
        //
        // WholeDesktop is the owner deciding that a general skill is the point: the
        // pointer goes anywhere on the virtual desktop and the keyboard goes to whatever
        // has focus, the way it does for the person sitting there. Containment stops
        // being categorical at that moment. What remains is the owner's explicit grant,
        // the command-surface refusal below, the input budget, the audit trail, and the
        // latched emergency stop -- and none of those is a proof that a general input
        // capability cannot eventually reach something it should not.
        enum class InputScope
        {
            ApprovedApplications,
            WholeDesktop
        };

        bool pointer = false;
        bool keyboard = false;
        bool applicationLaunch = false;
        // Permission to aim at a point Revia chose rather than at an element the
        // vision-to-UIA resolver re-verified. The point must still land inside the
        // target application's own window.
        bool rawCoordinates = false;
        // Separate authority, for the same reason autonomous research is separate from
        // ordinary lookup: delegating a task is not standing consent to drive the
        // machine whenever she feels like it.
        bool autonomous = false;
        InputScope scope = InputScope::ApprovedApplications;
        // A shell reached by keystroke is still model text reaching a shell. Command
        // interpreters, script hosts, and the chords that summon them are refused unless
        // the owner separately says otherwise, so that stays a decision rather than a
        // side effect of granting the desktop.
        bool allowCommandSurfaces = false;
        int maxInputActionsPerMinute = 30;
        int minimumInputIntervalMs = 120;
        std::size_t maxTypedCharacters = 512;

        [[nodiscard]] bool AnyEnabled() const
        {
            return pointer || keyboard || applicationLaunch;
        }
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
    DesktopControl desktopControl;
};

[[nodiscard]] std::string ToString(ActionType value);
[[nodiscard]] std::string ToString(RiskLevel value);
[[nodiscard]] std::string ToString(PolicyVerdict value);
[[nodiscard]] std::string ToString(ExecutionMode value);
[[nodiscard]] std::string ToString(CapabilitySettings::DesktopControl::InputScope value);
[[nodiscard]] CapabilitySettings::DesktopControl::InputScope InputScopeFromString(
    const std::string& value);
[[nodiscard]] ActionType ActionTypeFromString(const std::string& value);
[[nodiscard]] RiskLevel RiskLevelFromString(const std::string& value);
[[nodiscard]] ExecutionMode ExecutionModeFromString(const std::string& value);
[[nodiscard]] RiskLevel RiskForAction(ActionType value);
// UI Automation and desktop operation both drive an application, but only the second
// synthesizes input or starts a process, so they are gated separately.
[[nodiscard]] bool IsUiAutomationAction(ActionType value);
[[nodiscard]] bool IsDesktopControlAction(ActionType value);
[[nodiscard]] bool IsSynthesizedInputAction(ActionType value);
// True for a request Revia raised on her own rather than one a user turn asked for.
// The prefix convention is shared with internet research.
[[nodiscard]] bool IsAutonomousRequest(const std::string& requestedBy);
// One parsed keyboard chord: modifiers plus exactly one key. Parsing says whether the
// chord is well formed. Whether it is *permitted* is a policy question that depends on
// the current input scope, and lives in the three predicates below.
struct KeyChord
{
    std::string normalized;
    std::vector<int> modifierVirtualKeys;
    int virtualKey = 0;
    bool usesWindowsKey = false;
};

// Accepts "Ctrl + Shift+S" as "ctrl+shift+s". Modifier order is normalized to
// win+ctrl+alt+shift and key aliases collapse to one spelling, so a chord cannot be
// spelled around a predicate below. Rejects unknown names and anything that is not
// modifiers plus one key.
[[nodiscard]] bool ParseKeyChord(
    const std::string& value, KeyChord& outChord, std::string& outError);

// Moves focus to a different application. Refused while input is confined to one
// approved application, because leaving it is exactly what confinement means; allowed
// on the whole desktop, where switching windows is ordinary use.
[[nodiscard]] bool IsApplicationSwitchingChord(const std::string& normalizedChord);
// Opens a run box, a search field, or a menu that offers a terminal. Refused unless the
// owner has allowed command surfaces, in either scope.
[[nodiscard]] bool IsCommandSurfaceChord(const std::string& normalizedChord);
// Refused everywhere. Windows will not synthesize the secure attention sequence anyway,
// so accepting it would only be a lie about what happened.
[[nodiscard]] bool IsAlwaysRefusedChord(const std::string& normalizedChord);
// Executables that are a command interpreter or a script host. Input into one of these
// is model text reaching a shell however it got there, so it needs the same permission
// the chords do.
[[nodiscard]] bool IsCommandSurfaceExecutable(const std::string& executableName);
[[nodiscard]] std::string NewActionId();
[[nodiscard]] std::filesystem::path Utf8ToPath(const std::string& value);
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& value);

} // namespace revia::actions
