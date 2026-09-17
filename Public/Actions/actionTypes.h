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

// How much is actually known about what a pointer action is aimed at.
//
// Three different amounts of evidence, not three spellings of one thing. Collapsing them
// is how a click ends up somewhere nobody authorized.
//
// UiaElement is the strongest and the oldest: vision picked a region, the resolver
// matched it to one UI Automation element, and the executor re-finds that exact runtime
// identity and uses its current bounds. The coordinate captured when the decision was
// made is never the coordinate clicked.
//
// VisualRegion is for the interfaces UI Automation cannot describe -- a game, an Unreal
// or UMG surface, a canvas, a custom HUD, an Electron window with a bare tree. There is
// no element to re-find, so what gets verified instead is the *observation*: the same
// window, still the same size and place, still the newest thing looked at, with the point
// derived from the region at the last possible moment. Weaker than UiaElement, and
// deliberately stronger than a bare coordinate -- a region bound to one observation of
// one window is a claim that can be checked, and an arbitrary point is not.
//
// RawCoordinate is a point with no verified visual binding behind it. It keeps its own
// switch and is not either of the other two.
enum class TargetResolutionKind
{
    None,
    UiaElement,
    VisualRegion,
    RawCoordinate
};

enum class RiskLevel
{
    ReadOnly = 0,
    ReversibleWrite = 1,
    Destructive = 2
};

// What an action would actually cause, as opposed to how it is performed.
//
// RiskLevel describes the mechanism: every click is a click, so every click is the same
// reversible write. That is true and useless. Clicking a tab and clicking "Confirm
// purchase" are the same keystroke-level event and nothing alike in consequence, and the
// difference lives in what is being clicked, not in the clicking.
//
// Ordered by severity, because the ceiling below is a comparison.
enum class ConsequenceClass
{
    Observation = 0,
    // Ordinary interaction with no lasting effect outside the window.
    Routine = 1,
    // Changes the user's own documents or data.
    UserContent = 2,
    // Leaves the machine. Hard to retract once it has gone.
    ExternalMessage = 3,
    Financial = 4,
    // Permanent loss.
    Destructive = 5,
    // Credentials, permissions, account state -- the things that enable everything else.
    AccountOrSecurity = 6,
    // Arbitrary execution.
    CommandSurface = 7
};

enum class PolicyVerdict
{
    Allowed,
    RequiresConfirmation,
    Blocked
};

// What a person said when they were asked.
//
// Three answers rather than two, because "yes" and "yes, and stop asking me for this
// task" are different consents and only the person can tell them apart. Driving a
// browser takes a focus, a chord, a type and an enter, and being asked four times for
// one sentence is how a safety prompt turns into a thing people click through without
// reading -- which is worse than not asking.
//
// AllowForThisTask is bounded by construction and the bounds are not negotiable: it
// lives for one goal run, is never written anywhere, raises no permission, and covers
// only work at or below the risk level that was actually shown. Anything above it asks
// again. It also has no effect on the consequence ceiling, which is a separate gate
// inside the executor -- a send, a purchase or a delete still stops for its own explicit
// yes, as DECISION-REVIA-0007 requires.
enum class ConfirmationChoice
{
    Decline,
    Allow,
    AllowForThisTask
};

[[nodiscard]] inline bool Granted(const ConfirmationChoice choice)
{
    return choice != ConfirmationChoice::Decline;
}

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
        // Replaces an earlier `visionResolved` bool. A bool could only say "vision was
        // involved", which was the same answer for an exact UIA element and for a bare
        // region, and every reader of it meant the first. Asking the question properly
        // is what stops a visual region from being treated as a re-findable element.
        TargetResolutionKind kind = TargetResolutionKind::None;
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

        // Which observation a VisualRegion target belongs to, and what the machine
        // looked like when it was taken. Unused by the other kinds.
        //
        // Every field below is stamped by the runtime from its own observation. None of
        // it is ever read out of model output, for the reason TargetBinding gives about
        // its own id: evidence that could be supplied from outside would be an
        // authorization the model wrote for itself. The model says where to look; the
        // runtime says what was there.
        std::string observationId;
        // Strictly increasing across the process. An older generation means something
        // has been observed since, so the region describes a screen that has been
        // replaced.
        std::uint64_t observationGeneration = 0;
        // The observation's digest. Audit evidence rather than a gate -- see
        // DesktopObservation::Fingerprint.
        std::string screenDigest;
        // Stable window identity at observation time. void* rather than HWND so the
        // comparison can be tested without Windows, matching TargetBinding.
        void* observedWindow = nullptr;
        std::uint32_t observedProcessId = 0;
        std::string observedApplication;
        int observedWindowLeft = 0;
        int observedWindowTop = 0;
        int observedWindowRight = 0;
        int observedWindowBottom = 0;
        // Milliseconds on the steady clock. Stored as a plain integer so this header
        // stays free of <chrono> and so the value survives being written to an audit
        // record unchanged.
        std::uint64_t observedAtMs = 0;
        // Set when a UIA resolution was tried for this target and did not produce one,
        // with the resolver's own reason. This is the difference between "UI Automation
        // had nothing usable here" and "nobody looked", and only the first is a reason
        // to fall back to pixels.
        bool uiaAttempted = false;
        std::string uiaFailure;

        // True only for an exact UI Automation runtime identity the executor can re-find.
        // Named rather than compared inline because every caller that used to read
        // `visionResolved` meant precisely this, and must keep meaning it.
        [[nodiscard]] bool IsUiaElementTarget() const
        {
            return kind == TargetResolutionKind::UiaElement;
        }
        // True for a region bound to one observation of one window.
        [[nodiscard]] bool IsVisualRegionTarget() const
        {
            return kind == TargetResolutionKind::VisualRegion;
        }
        [[nodiscard]] bool HasRegion() const
        {
            return regionRight > regionLeft && regionBottom > regionTop;
        }
        // The point a region resolves to. Derived rather than stored, so that nothing
        // can carry a stale coordinate: callers ask at the moment of use.
        [[nodiscard]] int RegionCentreX() const
        {
            return regionLeft + (regionRight - regionLeft) / 2;
        }
        [[nodiscard]] int RegionCentreY() const
        {
            return regionTop + (regionBottom - regionTop) / 2;
        }
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
        // Where a visually grounded drag ends, as a region rather than a point, for the
        // same reason the start is one. Both ends resolve to coordinates at execution.
        int endRegionLeft = 0;
        int endRegionTop = 0;
        int endRegionRight = 0;
        int endRegionBottom = 0;

        [[nodiscard]] bool HasEndRegion() const
        {
            return endRegionRight > endRegionLeft && endRegionBottom > endRegionTop;
        }
        [[nodiscard]] int EndRegionCentreX() const
        {
            return endRegionLeft + (endRegionRight - endRegionLeft) / 2;
        }
        [[nodiscard]] int EndRegionCentreY() const
        {
            return endRegionTop + (endRegionBottom - endRegionTop) / 2;
        }
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
        // Permission to act on a target grounded in a fresh screen observation when UI
        // Automation cannot provide an exact element.
        //
        // This is the narrower of the two and is not a weaker spelling of the one above.
        // rawCoordinates means "aim wherever you decided"; this means "aim at the thing
        // you just looked at, in the window you just looked at, while it is still the
        // newest thing looked at and has not moved or resized". A target that fails any
        // of those is refused rather than clicked, which is what makes it a different
        // permission and not a euphemism for the same one.
        //
        // It exists because a game, an Unreal or UMG surface, a canvas and a bare
        // Electron tree expose nothing for the resolver to match, and the alternative to
        // this is granting arbitrary coordinates to reach them -- strictly more
        // authority for strictly less evidence.
        bool visualTargeting = false;
        // Separate authority, for the same reason autonomous research is separate from
        // ordinary lookup: delegating a task is not standing consent to drive the
        // machine whenever she feels like it.
        bool autonomous = false;
        // The most consequential thing she may commit without being stopped. It is a
        // ceiling on the *target*, evaluated at the moment of injection, and it only
        // ever adds refusals: an action still has to pass the mode, the scope, the
        // capability switches and the risk ceiling first. Raising it is a deliberate
        // act, and OwnerFullAccess does not raise it.
        ConsequenceClass maxUnconfirmedConsequence = ConsequenceClass::Routine;
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
[[nodiscard]] std::string ToString(ConsequenceClass value);
// Audit spelling for the targeting route that actually executed. Stable strings: an
// audit trail that renames its own categories cannot be read across a version.
[[nodiscard]] std::string ToString(TargetResolutionKind value);
[[nodiscard]] ConsequenceClass ConsequenceClassFromString(const std::string& value);

// What a named control would do if it were activated.
//
// Read honestly: this is a tripwire, not a boundary. It matches English words in an
// accessible name, so it catches "Send", "Delete account" and "Confirm purchase", and it
// will miss an unlabelled icon, another language, and any wording nobody thought of.
// A control it does not recognize classifies as Routine.
//
// That is why it may only ever *raise* the required authority and never lower it. It is
// worth having because the cases it does catch are the expensive ones, and it is worth
// being plain about because a check that is trusted for more than it does is worse than
// no check at all.
//
// isPasswordField comes from UI Automation rather than from the name, and is the one
// signal here that is not a guess.
[[nodiscard]] ConsequenceClass ClassifyControlConsequence(
    const std::string& controlName,
    bool isPasswordField = false);

[[nodiscard]] std::string ToString(CapabilitySettings::DesktopControl::InputScope value);
[[nodiscard]] CapabilitySettings::DesktopControl::InputScope InputScopeFromString(
    const std::string& value);
[[nodiscard]] ActionType ActionTypeFromString(const std::string& value);
[[nodiscard]] RiskLevel RiskLevelFromString(const std::string& value);
[[nodiscard]] ExecutionMode ExecutionModeFromString(const std::string& value);
// One authoritative list of what the system can actually do.
//
// The planner prompt and the parser used to keep separate lists, and they drifted: seven
// action types existed and could be executed while the planner had never been told they
// were there, so a goal could not reach them. Anything that needs to name the vocabulary
// derives it from here, and a test asserts the two agree.
[[nodiscard]] const std::vector<ActionType>& AllActionTypes();
// Comma-separated canonical names, for a prompt. `readOnlyOnly` narrows it to the
// actions that observe without changing anything, which is what a verification step is
// allowed to use.
[[nodiscard]] std::string ActionVocabulary(bool readOnlyOnly = false);

[[nodiscard]] RiskLevel RiskForAction(ActionType value);
// Actions that must be agreed to one at a time, whatever else has been agreed to.
//
// A standing yes is bounded by risk level, and risk level alone is not enough here.
// MoveToRecycleBin is classified ReversibleWrite -- correctly, because the recycle bin
// can be emptied back out -- so a yes given for creating a folder would otherwise cover
// deleting one, which is not what anybody means by "don't ask again". FormatGoalPlan
// already marks this action specially for the same reason, noting that recycling is
// reversible_write and so "nothing else in the pipeline makes it stand out".
//
// This is a floor under a convenience, not a security boundary: capability policy, the
// consequence ceiling, the rate limiter and the audit trail all still apply as before.
[[nodiscard]] bool AlwaysNeedsItsOwnConfirmation(ActionType value);
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
