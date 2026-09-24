#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Planning/goalPlanner.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/desktopActionRateLimiter.h"
#include "Policy/desktopInputGuard.h"
#include "Policy/permissionStore.h"
#include "Vision/visionActionParser.h"
#include "Windows/desktopControlExecutor.h"
#include "Windows/desktopObserver.h"
#include "Windows/targetBinding.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

using namespace revia::actions;
using revia::tests::Check;

// Desktop operation is the capability with the widest blast radius, so these tests are
// about what is refused. The one thing they cannot cover is the executor itself:
// synthesizing real input would move the developer's actual pointer, so the SendInput
// path is exercised by hand and recorded as such rather than faked here.

nlohmann::json BaseSettings(const std::filesystem::path& root)
{
    return {
        {"mode", "supervised"},
        {"approvedRoots", {PathToUtf8(root)}},
        {"approvedApplications", {"notepad.exe"}},
        {"approvedControls", {{"notepad.exe", {"File"}}}},
        {"autoApproveRiskThrough", "read_only"},
        {"createMissingApprovedRoots", false}};
}

struct PolicyFixture
{
    revia::tests::ScopedTestDirectory directory;
    nlohmann::json settings = BaseSettings(directory.root);

    void Allow(
        const bool pointer,
        const bool keyboard,
        const bool launch,
        const bool raw = false,
        const bool autonomous = false,
        const bool visual = false)
    {
        settings["desktopControl"] = {
            {"pointer", pointer},
            {"keyboard", keyboard},
            {"applicationLaunch", launch},
            {"rawCoordinates", raw},
            {"visualTargeting", visual},
            {"autonomous", autonomous}};
    }

    // The wide scope, which only exists on top of a pointer that may choose its own
    // coordinates. Both prerequisites are set here so the tests below are about what
    // the scope does rather than about how it is spelled.
    void AllowWholeDesktop(const bool keyboard = true, const bool commandSurfaces = false)
    {
        Allow(true, keyboard, false, true);
        settings["desktopControl"]["scope"] = "whole_desktop";
        settings["desktopControl"]["allowCommandSurfaces"] = commandSurfaces;
    }

    [[nodiscard]] std::filesystem::path Write(const std::string& name = "capabilities.json") const
    {
        const std::filesystem::path path = directory.root / name;
        std::ofstream file(path);
        file << settings.dump(2);
        Check(file.good(), "Could not write the desktop control fixture policy.");
        return path;
    }

    [[nodiscard]] revia::policy::CapabilityPolicy Policy() const
    {
        revia::policy::PermissionStore store;
        CapabilitySettings loaded;
        std::string error;
        Check(store.Load(Write(), loaded, error), "Fixture policy did not load: " + error);
        return revia::policy::CapabilityPolicy(loaded);
    }
};

ActionRequest PointerRequest(const int x = 5, const int y = 5)
{
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::ClickPointer;
    request.application = "notepad.exe";
    request.input.x = x;
    request.input.y = y;
    request.input.hasPoint = true;
    return request;
}

ActionRequest KeyRequest(const std::string& chord)
{
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::PressKeys;
    request.application = "notepad.exe";
    request.input.keys = chord;
    return request;
}

// The same request with no application named: aimed at the desktop rather than at one
// approved window. Naming nothing is what asks for the wide scope.
ActionRequest ScreenRequest(const ActionType type)
{
    ActionRequest request;
    request.id = NewActionId();
    request.type = type;
    request.input.x = 400;
    request.input.y = 300;
    request.input.hasPoint = true;
    if (type == ActionType::DragPointer)
    {
        request.input.endX = 500;
        request.input.endY = 380;
        request.input.hasEndPoint = true;
    }
    if (type == ActionType::TypeText) request.value = "hello";
    if (type == ActionType::PressKeys) request.input.keys = "ctrl+s";
    if (type == ActionType::ScrollPointer) request.input.scrollClicks = -3;
    return request;
}

void TestDesktopControlIsOffUntilGranted()
{
    PolicyFixture fixture;
    const auto policy = fixture.Policy();
    Check(policy.Settings().desktopControl.pointer == false &&
        policy.Settings().desktopControl.keyboard == false &&
        policy.Settings().desktopControl.applicationLaunch == false,
        "A capability file without a desktopControl block granted desktop control.");

    const auto pointer = policy.Evaluate(PointerRequest());
    Check(pointer.verdict == PolicyVerdict::Blocked &&
        pointer.reason.find("Pointer control is disabled") != std::string::npos,
        "Pointer control was not blocked by default.");
    const auto keys = policy.Evaluate(KeyRequest("ctrl+s"));
    Check(keys.verdict == PolicyVerdict::Blocked &&
        keys.reason.find("Keyboard control is disabled") != std::string::npos,
        "Keyboard control was not blocked by default.");

    ActionRequest launch;
    launch.id = NewActionId();
    launch.type = ActionType::LaunchApplication;
    launch.application = "notepad.exe";
    Check(policy.Evaluate(launch).verdict == PolicyVerdict::Blocked,
        "Starting an application was not blocked by default.");
}

void TestUnapprovedApplicationIsRefused()
{
    PolicyFixture fixture;
    fixture.Allow(true, true, true, true);
    const auto policy = fixture.Policy();
    ActionRequest request = PointerRequest();
    request.application = "cmd.exe";
    const auto decision = policy.Evaluate(request);
    Check(decision.verdict == PolicyVerdict::Blocked &&
        decision.reason.find("approved application list") != std::string::npos,
        "Desktop control reached an application outside the approved list.");

    request.application = "..\\cmd.exe";
    Check(policy.Evaluate(request).verdict == PolicyVerdict::Blocked,
        "A path was accepted where an executable name was required.");
}

void TestRawCoordinatesNeedTheirOwnPermission()
{
    PolicyFixture fixture;
    fixture.Allow(true, false, false, false);
    const auto policy = fixture.Policy();
    const auto pointed = policy.Evaluate(PointerRequest());
    Check(pointed.verdict == PolicyVerdict::Blocked &&
        pointed.reason.find("chosen coordinate is disabled") != std::string::npos,
        "A chosen coordinate was accepted without the coordinate permission.");

    // A vision-resolved element needs no raw-coordinate permission: the executor
    // re-finds the element rather than trusting the coordinate.
    ActionRequest resolved = PointerRequest();
    resolved.input.hasPoint = false;
    resolved.resolution.kind = revia::actions::TargetResolutionKind::UiaElement;
    resolved.resolution.resolvedRuntimeId = "42.7";
    Check(policy.Evaluate(resolved).verdict == PolicyVerdict::RequiresConfirmation,
        "A vision-resolved click was not offered for confirmation.");

    ActionRequest aimless = PointerRequest();
    aimless.input.hasPoint = false;
    Check(policy.Evaluate(aimless).verdict == PolicyVerdict::Blocked,
        "A pointer action with no target at all was not blocked.");
}

// A target grounded in an observation, as the runtime would stamp one.
ActionRequest VisualRequest()
{
    ActionRequest request;
    request.id = "visual-1";
    request.type = ActionType::ClickPointer;
    request.application = "notepad.exe";
    request.resolution.kind = TargetResolutionKind::VisualRegion;
    request.resolution.modelTarget = "large Play button in the centre of the game menu";
    request.resolution.regionLeft = 810;
    request.resolution.regionTop = 540;
    request.resolution.regionRight = 1050;
    request.resolution.regionBottom = 630;
    request.resolution.modelConfidence = 0.94;
    request.resolution.observationId = "observation-7";
    request.resolution.observationGeneration = 7;
    request.resolution.observedWindow = reinterpret_cast<void*>(0x1234);
    request.resolution.observedProcessId = 4242;
    request.resolution.observedApplication = "notepad.exe";
    request.resolution.observedWindowLeft = 0;
    request.resolution.observedWindowTop = 0;
    request.resolution.observedWindowRight = 1920;
    request.resolution.observedWindowBottom = 1080;
    request.resolution.observedAtMs = 1000;
    request.resolution.uiaAttempted = true;
    request.resolution.uiaFailure = "No candidate elements were found in the window.";
    return request;
}

// The whole point of the change: three levels of evidence are three permissions, and
// neither of the two narrow ones is reachable through the other.
void TestVisualTargetingIsItsOwnPermission()
{
    // Chosen coordinates do not imply it. This is the direction that matters most: if
    // granting the wider permission quietly granted this one, the distinction would be
    // decorative.
    PolicyFixture rawOnly;
    rawOnly.Allow(true, false, false, /*raw=*/true, false, /*visual=*/false);
    const auto rawPolicy = rawOnly.Policy();
    const auto refused = rawPolicy.Evaluate(VisualRequest());
    Check(refused.verdict == PolicyVerdict::Blocked &&
        refused.reason.find("only see") != std::string::npos,
        "Chosen coordinates silently granted visual targeting: " + refused.reason);

    // And it does not imply them. A visual target is usable with the wider permission
    // switched off, which is what makes it narrower rather than a euphemism.
    PolicyFixture visualOnly;
    visualOnly.Allow(true, false, false, /*raw=*/false, false, /*visual=*/true);
    const auto visualPolicy = visualOnly.Policy();
    Check(visualPolicy.Evaluate(VisualRequest()).verdict ==
        PolicyVerdict::RequiresConfirmation,
        "A visually grounded click was refused under its own permission.");
    const auto stillRefused = visualPolicy.Evaluate(PointerRequest());
    Check(stillRefused.verdict == PolicyVerdict::Blocked &&
        stillRefused.reason.find("chosen coordinate is disabled") != std::string::npos,
        "Visual targeting was accepted as permission to aim at a bare coordinate.");

    // A visual target is never an exact UIA element, whatever else it is. Every check
    // that used to read visionResolved means that, and must keep meaning it.
    Check(!VisualRequest().resolution.IsUiaElementTarget(),
        "A visual region claimed to be a re-findable UI Automation element.");
}

// What a visual target has to carry before it is one at all.
void TestAVisualTargetMustBeGroundedInSomething()
{
    PolicyFixture fixture;
    fixture.Allow(true, false, false, false, false, /*visual=*/true);
    const auto policy = fixture.Policy();

    ActionRequest noRegion = VisualRequest();
    noRegion.resolution.regionRight = noRegion.resolution.regionLeft;
    Check(policy.Evaluate(noRegion).verdict == PolicyVerdict::Blocked,
        "A visual target with no region was accepted.");

    // Nothing observed it. Without this the kind would be doing no work except skipping
    // the raw-coordinate switch, which is exactly the hole to avoid.
    ActionRequest unobserved = VisualRequest();
    unobserved.resolution.observationGeneration = 0;
    unobserved.resolution.observationId.clear();
    unobserved.resolution.observedWindow = nullptr;
    const auto ungrounded = policy.Evaluate(unobserved);
    Check(ungrounded.verdict == PolicyVerdict::Blocked &&
        ungrounded.reason.find("not bound to an observation") != std::string::npos,
        "A visual target with no observation behind it was accepted.");

    // A point riding along under the narrower permission. The point would be what the
    // executor acted on, and it is not what was authorized.
    ActionRequest smuggled = VisualRequest();
    smuggled.input.hasPoint = true;
    smuggled.input.x = 10;
    smuggled.input.y = 10;
    const auto refusedPoint = policy.Evaluate(smuggled);
    Check(refusedPoint.verdict == PolicyVerdict::Blocked &&
        refusedPoint.reason.find("not a chosen point") != std::string::npos,
        "A chosen coordinate travelled under visual targeting: " + refusedPoint.reason);

    ActionRequest overconfident = VisualRequest();
    overconfident.resolution.modelConfidence = 1.4;
    Check(policy.Evaluate(overconfident).verdict == PolicyVerdict::Blocked,
        "A visual target with impossible confidence was accepted.");

    // A drag deals in two regions when it is visual, and needs both.
    ActionRequest halfDrag = VisualRequest();
    halfDrag.type = ActionType::DragPointer;
    Check(policy.Evaluate(halfDrag).verdict == PolicyVerdict::Blocked,
        "A visual drag with only a start region was accepted.");
    ActionRequest wholeDrag = halfDrag;
    wholeDrag.input.endRegionLeft = 200;
    wholeDrag.input.endRegionTop = 200;
    wholeDrag.input.endRegionRight = 260;
    wholeDrag.input.endRegionBottom = 240;
    Check(policy.Evaluate(wholeDrag).verdict == PolicyVerdict::RequiresConfirmation,
        "A visual drag naming both regions was refused.");
}

// A region belongs to one observation of one window. These are the ways that stops
// being true, and every one of them has to end the action rather than move the pointer.
void TestAStaleVisualTargetIsNotClicked()
{
    using revia::actions::windows::CompareVisualTarget;
    using revia::actions::windows::VisualTargetFacts;

    const ActionRequest request = VisualRequest();
    const auto factsNow = []()
    {
        VisualTargetFacts facts;
        facts.latestGeneration = 7;
        facts.nowMs = 1500;
        facts.foregroundWindow = reinterpret_cast<void*>(0x1234);
        facts.foregroundProcessId = 4242;
        facts.windowBoundsKnown = true;
        facts.windowLeft = 0;
        facts.windowTop = 0;
        facts.windowRight = 1920;
        facts.windowBottom = 1080;
        return facts;
    };

    Check(CompareVisualTarget(request.resolution, factsNow()).empty(),
        "A target that still describes the screen was called stale.");

    // Something has been looked at since. This is what catches the changes geometry
    // cannot see: navigation, a scroll, a modal opening inside the same window.
    VisualTargetFacts superseded = factsNow();
    superseded.latestGeneration = 8;
    Check(CompareVisualTarget(request.resolution, superseded)
            .find("observed again") != std::string::npos,
        "A target from a superseded observation was still considered current.");

    VisualTargetFacts elsewhere = factsNow();
    elsewhere.foregroundWindow = reinterpret_cast<void*>(0x9999);
    Check(!CompareVisualTarget(request.resolution, elsewhere).empty(),
        "A target was accepted while a different window was in front.");

    VisualTargetFacts reused = factsNow();
    reused.foregroundProcessId = 5;
    Check(!CompareVisualTarget(request.resolution, reused).empty(),
        "A recycled window handle in another process was accepted.");

    VisualTargetFacts moved = factsNow();
    moved.windowLeft += 40;
    moved.windowRight += 40;
    Check(CompareVisualTarget(request.resolution, moved)
            .find("moved or been resized") != std::string::npos,
        "A window that moved did not invalidate the region measured against it.");

    VisualTargetFacts resized = factsNow();
    resized.windowBottom = 700;
    Check(!CompareVisualTarget(request.resolution, resized).empty(),
        "A window that was resized did not invalidate its region.");

    VisualTargetFacts late = factsNow();
    late.nowMs = request.resolution.observedAtMs +
        revia::actions::windows::VisualTargetFreshnessMs + 1;
    Check(CompareVisualTarget(request.resolution, late)
            .find("too old") != std::string::npos,
        "A target held past the freshness limit was still acted on.");

    // The window shrank around the region rather than moving. Checked separately
    // because the point, not the rectangle, is what the pointer would go to.
    ActionRequest outside = VisualRequest();
    outside.resolution.regionLeft = 4000;
    outside.resolution.regionRight = 4200;
    Check(!CompareVisualTarget(outside.resolution, factsNow()).empty(),
        "A region outside the window it was seen in was accepted.");

    // Kinds that are not visual must not be answerable by this check at all, or a raw
    // coordinate could be laundered through it.
    ActionRequest raw = VisualRequest();
    raw.resolution.kind = TargetResolutionKind::RawCoordinate;
    Check(!CompareVisualTarget(raw.resolution, factsNow()).empty(),
        "A raw coordinate was validated as a visual target.");
}

// Vision could previously say only "invoke this control" or "put this text in it",
// which is nothing at all about a game.
void TestVisionCanProposeTheOrdinaryVocabulary()
{
    const revia::vision::VisionActionParser parser;

    const auto click = parser.Parse(
        R"({"action":"click_pointer","target_description":"large Play button",)"
        R"("region":{"left":810,"top":540,"right":1050,"bottom":630},"confidence":0.94})");
    Check(click.succeeded && click.intent.action == ActionType::ClickPointer &&
        click.intent.region.left == 810,
        "Vision could not propose a click on something it can see: " + click.reason);

    // A keystroke aims at nothing, so it needs no region and no visual authority. This
    // is the transferable route -- ctrl+l focuses an address bar in every browser --
    // and requiring a rectangle for it would be asking for the wrong permission.
    const auto keys = parser.Parse(
        R"({"action":"press_keys","keys":"ctrl+l","confidence":0.98})");
    Check(keys.succeeded && keys.intent.action == ActionType::PressKeys &&
        keys.intent.keys == "ctrl+l",
        "Vision could not propose a key chord without a region: " + keys.reason);

    const auto scroll = parser.Parse(
        R"({"action":"scroll_pointer","target_description":"results list","scroll":3,)"
        R"("region":{"left":10,"top":10,"right":600,"bottom":800},"confidence":0.8})");
    Check(scroll.succeeded && scroll.intent.scrollClicks == 3,
        "Vision could not propose a scroll: " + scroll.reason);

    // A point the model wrote is not a visual target. Accepting one would turn this
    // parser into a way around the raw-coordinate permission.
    const auto pointed = parser.Parse(
        R"({"action":"click_pointer","target_description":"button","x":927,"y":587,)"
        R"("region":{"left":810,"top":540,"right":1050,"bottom":630},"confidence":0.9})");
    Check(!pointed.succeeded && pointed.reason.find("never a point") != std::string::npos,
        "Vision was allowed to name a coordinate directly: " + pointed.reason);

    const auto noRegion = parser.Parse(
        R"({"action":"click_pointer","target_description":"button","confidence":0.9})");
    Check(!noRegion.succeeded,
        "A pointer proposal with no region was accepted.");

    const auto anonymous = parser.Parse(
        R"({"action":"click_pointer","region":{"left":1,"top":1,"right":9,"bottom":9},)"
        R"("confidence":0.9})");
    Check(!anonymous.succeeded,
        "A pointer proposal that described nothing was accepted.");

    // Still closed to everything else. Widening the vocabulary is not opening it.
    const auto forbidden = parser.Parse(
        R"({"action":"move_file","source":"a","destination":"b","confidence":0.9})");
    Check(!forbidden.succeeded,
        "Vision was allowed to propose a filesystem action.");

    const auto uia = parser.Parse(
        R"({"action":"invoke_control","target_name":"Save",)"
        R"("region":{"left":1,"top":1,"right":9,"bottom":9},"confidence":0.9})");
    Check(uia.succeeded && uia.intent.action == ActionType::InvokeControl,
        "The original UI Automation route stopped working: " + uia.reason);
}

// Edge refused every keystroke with "The caret left view_1021 before typing began".
// The caret had not left: the wait loop immediately above had just proved focus was on
// it using that same runtime id. Chromium had regenerated the id in between, and the
// check was a raw string comparison.
void TestARegeneratedRuntimeIdIsNotAMovedCaret()
{
    using revia::actions::windows::SameControl;
    using revia::actions::windows::TargetBinding;

    const auto omnibox = []()
    {
        TargetBinding binding;
        binding.valid = true;
        binding.window = reinterpret_cast<void*>(0x2001);
        binding.processId = 7788;
        binding.runtimeId = "42.1.7";
        binding.automationId = "view_1021";
        binding.controlName = "Address and search bar";
        binding.controlType = 50004;
        binding.isPassword = false;
        binding.left = 120;
        binding.top = 60;
        binding.right = 980;
        binding.bottom = 92;
        return binding;
    };

    Check(SameControl(omnibox(), omnibox()),
        "A control did not match itself.");

    // The actual failure, reproduced: everything describes the same control and only
    // the volatile id differs.
    TargetBinding rebuilt = omnibox();
    rebuilt.runtimeId = "42.1.9";
    Check(SameControl(omnibox(), rebuilt),
        "A regenerated accessibility id was treated as the caret having moved, which is "
        "what refused every keystroke into Edge.");

    // And the protection it must not cost. The incident behind this check was two text
    // fields in one window and 560 characters going into the wrong one.
    TargetBinding otherField = omnibox();
    otherField.runtimeId = "42.1.9";
    otherField.top = 300;
    otherField.bottom = 332;
    Check(!SameControl(omnibox(), otherField),
        "A different field at a different position was accepted as the same control.");

    TargetBinding renamed = omnibox();
    renamed.runtimeId.clear();
    renamed.controlName = "Search the web";
    Check(!SameControl(omnibox(), renamed),
        "A differently named control was accepted as the same one.");

    TargetBinding secret = omnibox();
    secret.runtimeId.clear();
    secret.isPassword = true;
    Check(!SameControl(omnibox(), secret),
        "A password field was accepted as the control that was authorized.");

    TargetBinding elsewhere = omnibox();
    elsewhere.runtimeId = "42.1.9";
    elsewhere.window = reinterpret_cast<void*>(0x3002);
    Check(!SameControl(omnibox(), elsewhere),
        "A control in another window was accepted.");

    TargetBinding reusedHandle = omnibox();
    reusedHandle.runtimeId = "42.1.9";
    reusedHandle.processId = 9;
    Check(!SameControl(omnibox(), reusedHandle),
        "A recycled window handle in another process was accepted.");

    TargetBinding unobserved;
    Check(!SameControl(omnibox(), unobserved) && !SameControl(unobserved, omnibox()),
        "An unobserved binding matched something.");
}

void TestRawCoordinatesCannotOutliveThePointer()
{
    PolicyFixture fixture;
    fixture.Allow(false, false, true, true);
    revia::policy::PermissionStore store;
    CapabilitySettings loaded;
    std::string error;
    Check(!store.Load(fixture.Write("orphan.json"), loaded, error) &&
        error.find("require pointer control") != std::string::npos,
        "Raw coordinates were accepted without pointer control.");

    PolicyFixture autonomy;
    autonomy.Allow(false, false, false, false, true);
    Check(!store.Load(autonomy.Write("orphan-autonomy.json"), loaded, error) &&
        error.find("at least one enabled desktop capability") != std::string::npos,
        "Autonomous desktop control was accepted with no capability to be autonomous with.");
}

void TestAutonomousDesktopWorkIsSeparatelyPermitted()
{
    PolicyFixture fixture;
    fixture.Allow(true, true, true, true, false);
    const auto policy = fixture.Policy();
    ActionRequest request = PointerRequest();
    request.requestedBy = "autonomous_activity/idle-1";
    const auto decision = policy.Evaluate(request);
    Check(decision.verdict == PolicyVerdict::Blocked &&
        decision.reason.find("separate permission") != std::string::npos,
        "Revia drove the desktop unprompted without the autonomous permission.");

    PolicyFixture granted;
    granted.Allow(true, true, true, true, true);
    Check(granted.Policy().Evaluate(request).verdict == PolicyVerdict::RequiresConfirmation,
        "Granting autonomous desktop control did not admit an autonomous request.");
}

void TestKeyChordsCannotLeaveTheApplication()
{
    PolicyFixture fixture;
    fixture.Allow(false, true, false);
    const auto policy = fixture.Policy();
    // Confined input may not address the desktop at all, so every Windows-key chord and
    // every window switch is refused while it is the scope.
    for (const std::string chord : {"win+e", "win+p", "meta+h"})
    {
        const auto decision = policy.Evaluate(KeyRequest(chord));
        Check(decision.verdict == PolicyVerdict::Blocked &&
            decision.reason.find("address the desktop") != std::string::npos,
            "A Windows-key chord was accepted while input was confined: " + chord);
    }
    for (const std::string chord : {"alt+tab", "ctrl+shift+esc", "ctrl+escape",
             "alt+escape", "win+tab"})
    {
        const auto decision = policy.Evaluate(KeyRequest(chord));
        Check(decision.verdict == PolicyVerdict::Blocked,
            "An application-switching chord was accepted while confined: " + chord);
    }
    Check(policy.Evaluate(KeyRequest("ctrl+alt+delete")).verdict == PolicyVerdict::Blocked,
        "The secure attention sequence was accepted.");
    for (const std::string chord : {"", "ctrl", "ctrl+", "ctrl+s+a", "ctrl+notakey"})
    {
        Check(policy.Evaluate(KeyRequest(chord)).verdict == PolicyVerdict::Blocked,
            "A malformed chord was accepted: '" + chord + "'");
    }
    Check(policy.Evaluate(KeyRequest("Ctrl + Shift+S")).verdict ==
        PolicyVerdict::RequiresConfirmation,
        "A well-formed chord was not offered for confirmation.");
}

void TestSwitchingChordsFollowTheScope()
{
    // Learning the desktop includes learning to change windows, so the chords that were
    // refused above become ordinary once the desktop is the scope.
    PolicyFixture fixture;
    fixture.AllowWholeDesktop();
    const auto policy = fixture.Policy();
    for (const std::string chord : {"alt+tab", "win+tab", "win+e", "ctrl+escape"})
    {
        ActionRequest request = KeyRequest(chord);
        request.application.clear();
        Check(policy.Evaluate(request).verdict == PolicyVerdict::RequiresConfirmation,
            "A window-switching chord was refused on the whole desktop: " + chord);
    }
    // The one that Windows will not synthesize stays refused in every scope, because
    // accepting it would only be a claim that something happened.
    ActionRequest attention = KeyRequest("ctrl+alt+delete");
    attention.application.clear();
    Check(policy.Evaluate(attention).verdict == PolicyVerdict::Blocked,
        "The secure attention sequence was accepted on the whole desktop.");
}

void TestKeyChordNormalization()
{
    KeyChord chord;
    std::string error;
    Check(ParseKeyChord("CTRL + shift + S", chord, error) &&
        chord.normalized == "ctrl+shift+s" && chord.modifierVirtualKeys.size() == 2 &&
        chord.virtualKey == 0x53 && !chord.usesWindowsKey,
        "A mixed-case chord did not normalize to ctrl+shift+s.");
    // Aliases collapse so a predicate cannot be sidestepped by spelling.
    Check(ParseKeyChord("alt+esc", chord, error) && chord.normalized == "alt+escape" &&
        IsApplicationSwitchingChord(chord.normalized),
        "alt+esc did not collapse onto the switching chord alt+escape.");
    Check(ParseKeyChord("Meta+R", chord, error) && chord.normalized == "win+r" &&
        chord.usesWindowsKey && IsCommandSurfaceChord(chord.normalized),
        "meta+r did not collapse onto the command surface win+r.");
    Check(ParseKeyChord("f12", chord, error) && chord.virtualKey == 0x7B,
        "A function key did not parse.");
}

void TestTypedTextIsBounded()
{
    PolicyFixture fixture;
    fixture.Allow(false, true, false);
    fixture.settings["desktopControl"]["maxTypedCharacters"] = 8;
    const auto policy = fixture.Policy();
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::TypeText;
    request.application = "notepad.exe";

    request.value = "short";
    Check(policy.Evaluate(request).verdict == PolicyVerdict::RequiresConfirmation,
        "Text inside the limit was not offered for confirmation.");
    request.value = "far too long for this limit";
    Check(policy.Evaluate(request).verdict == PolicyVerdict::Blocked,
        "Text past the configured limit was accepted.");
    request.value = std::string("a\x1b" "b");
    Check(policy.Evaluate(request).verdict == PolicyVerdict::Blocked,
        "Text containing a control character was accepted.");
    request.value.clear();
    Check(policy.Evaluate(request).verdict == PolicyVerdict::Blocked,
        "Empty text was accepted.");
}

void TestLaunchArgumentStaysInsideApprovedRoots()
{
    PolicyFixture fixture;
    fixture.Allow(false, false, true);
    const auto policy = fixture.Policy();
    ActionRequest request;
    request.id = NewActionId();
    request.type = ActionType::LaunchApplication;
    request.application = "notepad.exe";

    request.source = fixture.directory.root / "notes.txt";
    Check(policy.Evaluate(request).verdict == PolicyVerdict::RequiresConfirmation,
        "Opening a file inside an approved root was not offered for confirmation.");
    request.source = std::filesystem::temp_directory_path() / "elsewhere.txt";
    const auto outside = policy.Evaluate(request);
    Check(outside.verdict == PolicyVerdict::Blocked &&
        outside.reason.find("outside every approved root") != std::string::npos,
        "A launch argument escaped the approved roots.");
}

void TestConfinedInputNeedsAnApplication()
{
    PolicyFixture fixture;
    fixture.Allow(true, true, false, true);
    const auto policy = fixture.Policy();
    for (const ActionType type : {ActionType::MoveCursor, ActionType::ClickPointer,
             ActionType::DragPointer, ActionType::ScrollPointer, ActionType::PressKeys,
             ActionType::TypeText})
    {
        const auto decision = policy.Evaluate(ScreenRequest(type));
        Check(decision.verdict == PolicyVerdict::Blocked &&
            decision.reason.find("needs an executable name") != std::string::npos,
            "Screen-space input was accepted while input was confined: " + ToString(type));
    }
}

void TestWholeDesktopNeedsItsPrerequisites()
{
    revia::policy::PermissionStore store;
    CapabilitySettings loaded;
    std::string error;

    PolicyFixture noPointer;
    noPointer.Allow(false, true, false, false);
    noPointer.settings["desktopControl"]["scope"] = "whole_desktop";
    Check(!store.Load(noPointer.Write("no-pointer.json"), loaded, error) &&
        error.find("requires pointer control") != std::string::npos,
        "The whole desktop was granted without a pointer.");

    PolicyFixture noCoordinates;
    noCoordinates.Allow(true, true, false, false);
    noCoordinates.settings["desktopControl"]["scope"] = "whole_desktop";
    Check(!store.Load(noCoordinates.Write("no-coordinates.json"), loaded, error),
        "The whole desktop was granted to a pointer that may not choose a point.");

    PolicyFixture nonsense;
    nonsense.Allow(true, true, false, true);
    nonsense.settings["desktopControl"]["scope"] = "everything";
    Check(!store.Load(nonsense.Write("nonsense.json"), loaded, error) &&
        error.find("Unsupported desktop control scope") != std::string::npos,
        "An unreadable scope was not rejected.");

    // A file that never mentions the scope is the narrow one, so an older capability
    // file cannot acquire the desktop by omission.
    PolicyFixture silent;
    silent.Allow(true, true, false, true);
    Check(store.Load(silent.Write("silent.json"), loaded, error) &&
        loaded.desktopControl.scope ==
            CapabilitySettings::DesktopControl::InputScope::ApprovedApplications,
        "A capability file with no scope did not default to approved applications.");
}

void TestWholeDesktopAcceptsScreenSpaceInput()
{
    PolicyFixture fixture;
    fixture.AllowWholeDesktop();
    const auto policy = fixture.Policy();
    for (const ActionType type : {ActionType::MoveCursor, ActionType::ClickPointer,
             ActionType::DragPointer, ActionType::ScrollPointer, ActionType::PressKeys,
             ActionType::TypeText})
    {
        const auto decision = policy.Evaluate(ScreenRequest(type));
        Check(decision.verdict == PolicyVerdict::RequiresConfirmation,
            "The whole desktop refused screen-space input: " + ToString(type) +
                " (" + decision.reason + ")");
    }

    // A scroll turns the wheel wherever the pointer already is, so it is the one action
    // that needs no point at all.
    ActionRequest scroll = ScreenRequest(ActionType::ScrollPointer);
    scroll.input.hasPoint = false;
    Check(policy.Evaluate(scroll).verdict == PolicyVerdict::RequiresConfirmation,
        "A scroll at the current pointer position was refused.");

    // Everything else still has to say where it is going.
    ActionRequest aimless = ScreenRequest(ActionType::ClickPointer);
    aimless.input.hasPoint = false;
    Check(policy.Evaluate(aimless).verdict == PolicyVerdict::Blocked,
        "A click with no target at all was accepted on the whole desktop.");

    ActionRequest halfDrag = ScreenRequest(ActionType::DragPointer);
    halfDrag.input.hasEndPoint = false;
    Check(policy.Evaluate(halfDrag).verdict == PolicyVerdict::Blocked,
        "A drag with no end point was accepted.");

    // Naming an application still means the confined form, and that form still checks
    // the allowlist. Widening the scope did not delete the narrow one.
    ActionRequest named = PointerRequest();
    named.application = "cmd.exe";
    Check(policy.Evaluate(named).verdict == PolicyVerdict::Blocked,
        "The wide scope let a named request skip the approved application list.");
}

void TestCommandSurfacesStayOutOfReach()
{
    PolicyFixture fixture;
    fixture.AllowWholeDesktop();
    const auto policy = fixture.Policy();
    for (const std::string chord : {"win+r", "win+x", "win+s", "win+i"})
    {
        ActionRequest request = KeyRequest(chord);
        request.application.clear();
        const auto decision = policy.Evaluate(request);
        Check(decision.verdict == PolicyVerdict::Blocked &&
            decision.reason.find("command or settings") != std::string::npos,
            "A command-surface chord was accepted: " + chord);
    }

    // Naming a shell is the same request as reaching one by keystroke, so the approved
    // application list does not become a way around this.
    PolicyFixture named;
    named.Allow(true, true, false, true);
    named.settings["approvedApplications"] = {"notepad.exe", "cmd.exe"};
    named.settings["approvedControls"] = {{"notepad.exe", {"File"}}, {"cmd.exe", {"*"}}};
    ActionRequest shell = PointerRequest();
    shell.application = "cmd.exe";
    const auto shellDecision = named.Policy().Evaluate(shell);
    Check(shellDecision.verdict == PolicyVerdict::Blocked &&
        shellDecision.reason.find("command surface") != std::string::npos,
        "An approved command interpreter was reachable without the extra permission.");

    // And it is a permission, not a wall: the owner can decide otherwise.
    PolicyFixture allowed;
    allowed.AllowWholeDesktop(true, true);
    ActionRequest runBox = KeyRequest("win+r");
    runBox.application.clear();
    Check(allowed.Policy().Evaluate(runBox).verdict == PolicyVerdict::RequiresConfirmation,
        "Allowing command surfaces did not admit the run box.");
}

void TestConsequenceIsReadFromTheTargetNotTheVerb()
{
    // The point of the whole classifier: these are all the same click.
    Check(ClassifyControlConsequence("Select tab") == ConsequenceClass::Routine &&
        ClassifyControlConsequence("Run tests") == ConsequenceClass::Routine &&
        ClassifyControlConsequence("Save") == ConsequenceClass::UserContent &&
        ClassifyControlConsequence("Send") == ConsequenceClass::ExternalMessage &&
        ClassifyControlConsequence("Buy now") == ConsequenceClass::Financial &&
        ClassifyControlConsequence("Delete") == ConsequenceClass::Destructive &&
        ClassifyControlConsequence("Delete account") == ConsequenceClass::AccountOrSecurity,
        "Identical clicks on different controls were not told apart.");

    // Severity order decides: deleting an account is an account change, not a deletion.
    Check(ClassifyControlConsequence("Delete account") >
        ClassifyControlConsequence("Delete"),
        "A dangerous compound label lost to its own substring.");

    // The reliable signal outranks the label entirely.
    Check(ClassifyControlConsequence("", true) == ConsequenceClass::AccountOrSecurity &&
        ClassifyControlConsequence("Nickname", true) == ConsequenceClass::AccountOrSecurity,
        "A password field was classified from its name instead of its own flag.");

    // Substring matching would make each of these a false alarm. A gate that cries wolf
    // is a gate that gets raised until it stops meaning anything.
    Check(ClassifyControlConsequence("Display settings") == ConsequenceClass::Routine &&
        ClassifyControlConsequence("Undelete") == ConsequenceClass::Routine &&
        ClassifyControlConsequence("Repayment history") == ConsequenceClass::Routine &&
        ClassifyControlConsequence("Sendai") == ConsequenceClass::Routine,
        "Word-boundary matching failed and produced a false alarm.");

    // What it does not know, it does not guess about.
    Check(ClassifyControlConsequence("") == ConsequenceClass::Routine &&
        ClassifyControlConsequence("Frobnicate") == ConsequenceClass::Routine,
        "An unrecognized control was classified as something.");

    Check(ConsequenceClassFromString(ToString(ConsequenceClass::Financial)) ==
            ConsequenceClass::Financial &&
        ConsequenceClassFromString("nonsense") == ConsequenceClass::Routine,
        "The consequence class did not round-trip, or an unreadable one was permissive.");
}

void TestTheConsequenceCeilingLoadsAndDefaultsNarrow()
{
    revia::policy::PermissionStore store;
    CapabilitySettings loaded;
    std::string error;

    // A capability file that never mentions it gets the narrow default, so an older
    // file cannot acquire a wider ceiling by omission.
    PolicyFixture silent;
    silent.Allow(true, true, false, true);
    Check(store.Load(silent.Write("silent-ceiling.json"), loaded, error) &&
        loaded.desktopControl.maxUnconfirmedConsequence == ConsequenceClass::Routine,
        "A file with no consequence ceiling did not default to routine: " + error);

    PolicyFixture raised;
    raised.Allow(true, true, false, true);
    raised.settings["desktopControl"]["maxUnconfirmedConsequence"] = "external_message";
    Check(store.Load(raised.Write("raised.json"), loaded, error) &&
        loaded.desktopControl.maxUnconfirmedConsequence == ConsequenceClass::ExternalMessage,
        "A raised consequence ceiling did not load: " + error);

    PolicyFixture nonsense;
    nonsense.Allow(true, true, false, true);
    nonsense.settings["desktopControl"]["maxUnconfirmedConsequence"] = "whatever";
    Check(!store.Load(nonsense.Write("bad-ceiling.json"), loaded, error) &&
        error.find("consequence ceiling") != std::string::npos,
        "An unreadable consequence ceiling was accepted.");
}

void TestTheCeilingOnlyEverAddsRefusals()
{
    // Composition property: raising the ceiling to its maximum must not make anything
    // reachable that the mode, scope and capability switches already refused.
    PolicyFixture permissive;
    permissive.Allow(true, true, true, true);
    permissive.settings["mode"] = "owner_full_access";
    permissive.settings["desktopControl"]["maxUnconfirmedConsequence"] = "command_surface";
    const auto policy = permissive.Policy();

    ActionRequest unapproved = PointerRequest();
    unapproved.application = "cmd.exe";
    Check(policy.Evaluate(unapproved).verdict == PolicyVerdict::Blocked,
        "The highest consequence ceiling reached an unapproved application.");

    ActionRequest screenSpace = ScreenRequest(ActionType::ClickPointer);
    Check(policy.Evaluate(screenSpace).verdict == PolicyVerdict::Blocked,
        "The consequence ceiling substituted for the input scope.");

    // And OwnerFullAccess does not raise it: the ceiling is its own decision.
    PolicyFixture owner;
    owner.Allow(true, true, true, true);
    owner.settings["mode"] = "owner_full_access";
    revia::policy::PermissionStore store;
    CapabilitySettings loaded;
    std::string error;
    Check(store.Load(owner.Write("owner-ceiling.json"), loaded, error) &&
        loaded.desktopControl.maxUnconfirmedConsequence == ConsequenceClass::Routine,
        "OwnerFullAccess quietly raised the consequence ceiling.");
}

void TestThePlannerAndParserShareOneVocabulary()
{
    // Seven action types existed and could be executed while the planner had never been
    // told they were there, so a goal could not reach them. Two hand-maintained lists
    // will always drift eventually; this asserts they are the same list.
    const std::string prompt = revia::planning::GoalPlanner::PlannerPrompt();
    for (const ActionType type : AllActionTypes())
    {
        const std::string name = ToString(type);
        Check(ActionTypeFromString(name) == type,
            "The canonical name \"" + name + "\" does not parse back to its own type.");
        Check(prompt.find(name) != std::string::npos,
            "The planner was never told \"" + name + "\" exists.");
    }

    // A verification step must not be able to change anything, so the list offered for
    // checks has to contain only read-only actions.
    const std::string readOnly = ActionVocabulary(/*readOnlyOnly=*/true);
    for (const ActionType type : AllActionTypes())
    {
        const bool offered = readOnly.find(ToString(type)) != std::string::npos;
        const bool isReadOnly = RiskForAction(type) == RiskLevel::ReadOnly;
        Check(offered == isReadOnly,
            "The read-only vocabulary disagrees with the risk table about \"" +
                ToString(type) + "\".");
    }
    Check(readOnly.find("click_pointer") == std::string::npos,
        "A mutating action was offered as a verification step.");
}

void TestOwnerFullAccessRaisesOnlyTheCeiling()
{
    PolicyFixture fixture;
    fixture.Allow(true, true, true, true);
    fixture.settings["mode"] = "owner_full_access";
    const auto policy = fixture.Policy();
    Check(policy.Evaluate(PointerRequest()).verdict == PolicyVerdict::Allowed,
        "OwnerFullAccess still stopped to confirm reversible desktop work.");

    // Scope is not authority: the same mode must not reach an unapproved application,
    // a disabled capability, or a path outside the approved roots.
    ActionRequest elsewhere = PointerRequest();
    elsewhere.application = "cmd.exe";
    Check(policy.Evaluate(elsewhere).verdict == PolicyVerdict::Blocked,
        "OwnerFullAccess widened the approved application list.");

    // Every filesystem action Revia has today is reversible, so the ceiling admits them
    // all under this mode. The root check is what still refuses, and it must.
    ActionRequest recycle;
    recycle.id = NewActionId();
    recycle.type = ActionType::MoveToRecycleBin;
    recycle.source = std::filesystem::temp_directory_path() / "not-in-scope.txt";
    Check(policy.Evaluate(recycle).verdict == PolicyVerdict::Blocked,
        "OwnerFullAccess widened the approved roots.");

    PolicyFixture noHands;
    noHands.settings["mode"] = "owner_full_access";
    Check(noHands.Policy().Evaluate(PointerRequest()).verdict == PolicyVerdict::Blocked,
        "OwnerFullAccess granted desktop control that was never switched on.");
}

void TestStopGuardLatchesUntilResumed()
{
    revia::policy::DesktopInputGuard guard;
    Check(!guard.IsTripped(), "A new stop guard started tripped.");
    guard.Trip("first reason");
    guard.Trip("second reason");
    Check(guard.IsTripped() && guard.Reason() == "first reason",
        "The stop guard did not latch on the original reason.");
    Check(guard.Resume() && !guard.IsTripped() && guard.Reason().empty(),
        "Resuming did not clear the stop guard.");
    Check(!guard.Resume(), "Resuming an untripped guard claimed it had been stopped.");
}

void TestExecutorRefusesWithoutAStopPath()
{
    revia::actions::windows::DesktopControlExecutor executor(
        CapabilitySettings::DesktopControl{}, nullptr);
    Check(executor.Handles(ActionType::ClickPointer) &&
        executor.Handles(ActionType::LaunchApplication) &&
        !executor.Handles(ActionType::InvokeControl),
        "The desktop control executor claimed the wrong action types.");

    PolicyDecision decision;
    decision.verdict = PolicyVerdict::Allowed;
    const ActionResult refused = executor.Execute(PointerRequest(), decision);
    Check(!refused.succeeded && !refused.attempted &&
        refused.message.find("emergency stop") != std::string::npos,
        "The executor acted without an emergency stop available.");

    // A dry run is answered before any of that, because it touches nothing.
    ActionRequest rehearsal = PointerRequest();
    rehearsal.dryRun = true;
    const ActionResult dry = executor.Execute(rehearsal, decision);
    Check(dry.succeeded && dry.dryRun, "A desktop control dry run did not pass.");
}

void TestInputBudgetIsSeparateFromUiAutomation()
{
    revia::policy::DesktopActionRateLimiter limiter;
    limiter.Configure(2, 0, revia::policy::DesktopActionRateLimiter::Scope::DesktopControl);
    const auto now = std::chrono::steady_clock::now();
    std::string reason;

    ActionRequest invoke;
    invoke.type = ActionType::InvokeControl;
    invoke.application = "notepad.exe";
    for (int index = 0; index < 5; ++index)
    {
        Check(limiter.Admit(invoke, now, reason),
            "The desktop control budget consumed a UI Automation action.");
    }
    Check(limiter.Admit(PointerRequest(), now, reason), "The first click was refused.");
    Check(limiter.Admit(PointerRequest(), now, reason), "The second click was refused.");
    Check(!limiter.Admit(PointerRequest(), now, reason) &&
        reason.find("per-minute input budget") != std::string::npos,
        "The per-minute input budget did not stop the third click.");

    limiter.Configure(60, 500, revia::policy::DesktopActionRateLimiter::Scope::DesktopControl);
    Check(limiter.Admit(PointerRequest(), now, reason), "The first spaced click was refused.");
    Check(!limiter.Admit(PointerRequest(), now + std::chrono::milliseconds(100), reason) &&
        reason.find("this quickly") != std::string::npos,
        "The minimum input interval did not apply.");
}

void TestCommandsAndJsonParseIntoTypedRequests()
{
    const revia::planning::StructuredActionParser parser;

    const auto click = parser.ParseCommand(
        "/click \"notepad.exe\" \"Untitled\" \"120\" \"-40\" \"right\" \"2\"");
    Check(click.succeeded && click.request.type == ActionType::ClickPointer &&
        click.request.application == "notepad.exe" &&
        click.request.windowTitle == "Untitled" &&
        click.request.input.x == 120 && click.request.input.y == -40 &&
        click.request.input.hasPoint &&
        click.request.input.button ==
            ActionRequest::DesktopInput::PointerButton::Right &&
        click.request.input.clickCount == 2,
        "The click command did not parse into a typed request.");

    // The shared tokenizer drops an empty quoted field, so the command form always
    // names a window the way /set-text and /invoke-control do. The JSON form may omit
    // window_title to mean "any window of that application".
    const auto press = parser.ParseCommand(
        "/press \"notepad.exe\" \"Untitled\" \"ctrl+s\"");
    Check(press.succeeded && press.request.type == ActionType::PressKeys &&
        press.request.input.keys == "ctrl+s",
        "The press command did not parse into a typed request.");

    const auto launch = parser.ParseCommand("/launch \"notepad.exe\"");
    Check(launch.succeeded && launch.request.type == ActionType::LaunchApplication &&
        launch.request.source.empty(),
        "The launch command did not parse into a typed request.");

    const auto typed = parser.ParseJson(
        R"({"action":"type_text","application":"notepad.exe","value":"hello"})");
    Check(typed.succeeded && typed.request.type == ActionType::TypeText &&
        typed.request.value == "hello",
        "A type_text proposal did not parse into a typed request.");

    const auto scroll = parser.ParseJson(
        R"({"action":"scroll_pointer","application":"notepad.exe","scroll":-3})");
    Check(scroll.succeeded && scroll.request.input.scrollClicks == -3,
        "A scroll proposal did not carry its detents.");

    // Screen-space forms. An executable name never parses as a whole number, so the two
    // shapes of the same command never collide.
    const auto screenClick = parser.ParseCommand("/click \"820\" \"140\" \"right\"");
    Check(screenClick.succeeded && screenClick.request.application.empty() &&
        screenClick.request.input.x == 820 && screenClick.request.input.y == 140 &&
        screenClick.request.input.button ==
            ActionRequest::DesktopInput::PointerButton::Right,
        "The screen-space click command did not parse.");

    const auto drag = parser.ParseCommand("/drag \"10\" \"20\" \"90\" \"120\"");
    Check(drag.succeeded && drag.request.type == ActionType::DragPointer &&
        drag.request.input.hasPoint && drag.request.input.hasEndPoint &&
        drag.request.input.endX == 90 && drag.request.input.endY == 120,
        "The screen-space drag command did not parse.");

    const auto windowDrag = parser.ParseCommand(
        "/drag \"notepad.exe\" \"Untitled\" \"10\" \"20\" \"90\" \"120\" \"middle\"");
    Check(windowDrag.succeeded && windowDrag.request.application == "notepad.exe" &&
        windowDrag.request.input.endY == 120 &&
        windowDrag.request.input.button ==
            ActionRequest::DesktopInput::PointerButton::Middle,
        "The window-scoped drag command did not parse.");

    const auto screenPress = parser.ParseCommand("/press \"alt+tab\"");
    Check(screenPress.succeeded && screenPress.request.application.empty() &&
        screenPress.request.input.keys == "alt+tab",
        "The screen-space press command did not parse.");

    // Text that happens to look like a number is still text: the keyboard commands are
    // told apart by field count rather than by what the field contains.
    const auto numericText = parser.ParseCommand("/type \"2026\"");
    Check(numericText.succeeded && numericText.request.value == "2026" &&
        numericText.request.application.empty(),
        "Typing a number was mistaken for a window-scoped command.");

    const auto screenScroll = parser.ParseCommand("/scroll \"-4\"");
    Check(screenScroll.succeeded && screenScroll.request.input.scrollClicks == -4 &&
        screenScroll.request.application.empty(),
        "The screen-space scroll command did not parse.");

    // A desktop-aimed proposal parses; whether that scope exists is policy's answer,
    // not the parser's.
    const auto screenJson = parser.ParseJson(
        R"({"action":"click_pointer","x":1,"y":2})");
    Check(screenJson.succeeded && screenJson.request.application.empty(),
        "A screen-space proposal was rejected by the parser.");
    const auto launchJson = parser.ParseJson(R"({"action":"launch_application"})");
    Check(launchJson.recognized && !launchJson.succeeded,
        "A launch proposal without an executable was accepted.");

    // Rejections. A malformed command must not become a request with defaults.
    for (const std::string command : {
             "/click \"notepad.exe\" \"Untitled\" \"x\" \"4\"",
             "/click \"notepad.exe\" \"Untitled\"",
             "/click \"820\"",
             "/drag \"10\" \"20\" \"90\"",
             "/scroll \"notepad.exe\" \"Untitled\"",
             "/scroll \"sideways\"",
             "/launch"})
    {
        const auto rejected = parser.ParseCommand(command);
        Check(rejected.recognized && !rejected.succeeded,
            "A malformed desktop command was accepted: " + command);
    }
}

void TestRuntimeRegistersAndReportsDesktopControl()
{
    PolicyFixture fixture;
    fixture.Allow(true, true, true, true);
    const std::filesystem::path configuration = fixture.Write();
    ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configuration, fixture.directory.root / "audit.jsonl", error),
        "The action runtime did not initialize with desktop control: " + error);

    const nlohmann::json status = nlohmann::json::parse(runtime.StatusJson());
    Check(status.at("desktop_control").at("pointer") == true &&
        status.at("desktop_control").at("stopped") == false,
        "Desktop control state was missing from the runtime status.");

    runtime.StopDesktopControl("test stop");
    Check(runtime.DesktopControlStopped() &&
        runtime.DesktopControlStopReason() == "test stop",
        "The runtime did not carry the stop through to the guard.");
    const nlohmann::json stopped = nlohmann::json::parse(runtime.StatusJson());
    Check(stopped.at("desktop_control").at("stopped") == true,
        "A stopped desktop control was not visible in the runtime status.");

    // A stopped guard must not silently make the action look impossible: policy still
    // evaluates normally, and the refusal happens at the executor with its reason.
    Check(runtime.Evaluate(PointerRequest()).verdict == PolicyVerdict::RequiresConfirmation,
        "Stopping desktop control changed what policy decides.");
    Check(runtime.ResumeDesktopControl() && !runtime.DesktopControlStopped(),
        "Desktop control could not be resumed.");

    using InputScope = CapabilitySettings::DesktopControl::InputScope;
    // The wide scope round-trips through the editor and the store.
    Check(runtime.SetDesktopControl(
            true, true, false, true, false, false, InputScope::WholeDesktop, false, error) &&
        runtime.Settings().desktopControl.scope == InputScope::WholeDesktop,
        "The whole-desktop scope did not persist: " + error);

    // Persisted permission changes survive the reload and drop their subset authorities.
    Check(runtime.SetDesktopControl(
            false, true, false, true, true, true, InputScope::WholeDesktop, true, error),
        "Desktop control settings could not be written: " + error);
    const auto reloaded = runtime.Settings().desktopControl;
    Check(!reloaded.pointer && reloaded.keyboard && !reloaded.rawCoordinates &&
        reloaded.autonomous,
        "Withdrawing pointer control did not withdraw chosen coordinates with it.");
    // Visual targeting aims the pointer too, so it goes when the pointer goes. It is a
    // narrower authority than chosen coordinates, not an independent one.
    Check(!reloaded.visualTargeting,
        "Withdrawing pointer control left visual targeting behind.");
    Check(reloaded.scope == InputScope::ApprovedApplications,
        "The whole desktop survived the withdrawal of the pointer it was built on.");
    Check(runtime.SetDesktopControl(
            false, false, false, false, false, true, InputScope::ApprovedApplications,
            true, error) &&
        !runtime.Settings().desktopControl.autonomous &&
        !runtime.Settings().desktopControl.allowCommandSurfaces,
        "Autonomy or command surfaces survived the withdrawal of every capability.");

    Check(runtime.SetExecutionMode(ExecutionMode::OwnerFullAccess, error) &&
        runtime.Settings().mode == ExecutionMode::OwnerFullAccess,
        "Execution mode could not be changed: " + error);
}

// Rule of the house: verify the pipeline, not the function. This runs a real command
// through parser, policy, rate limiter, dispatcher, executor, and audit. It uses a dry
// run so the pipeline is exercised without moving the developer's actual pointer; the
// synthesized-input half of the executor is verified by hand.
void TestDesktopCommandTravelsTheWholePipeline()
{
    PolicyFixture fixture;
    fixture.Allow(true, true, true, true);
    const std::filesystem::path audit = fixture.directory.root / "audit.jsonl";
    ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(fixture.Write(), audit, error),
        "The action runtime did not initialize: " + error);

    auto parsed = runtime.ParseCommand(
        "/click \"notepad.exe\" \"Untitled\" \"7\" \"9\" \"left\" \"1\"");
    Check(parsed.recognized && parsed.succeeded, "The click command did not parse.");
    parsed.request.dryRun = true;
    const ActionOutcome outcome = runtime.Execute(parsed.request, true);
    Check(outcome.Succeeded() && outcome.result.dryRun,
        "A dry-run click did not reach the executor: " + outcome.Message());

    std::vector<nlohmann::json> records;
    {
        std::ifstream file(audit);
        std::string line;
        while (std::getline(file, line)) records.push_back(nlohmann::json::parse(line));
    }
    Check(records.size() == 2 && records[0].at("record_type") == "intent" &&
        records[1].at("record_type") == "result" &&
        records[1].at("action") == "click_pointer" &&
        records[1].at("succeeded") == true,
        "The desktop action did not produce an intent and a result record.");
    const nlohmann::json& desktop = records[1].at("desktop_input");
    Check(desktop.at("button") == "left" && desktop.at("click_count") == 1 &&
        desktop.at("requested_x") == 7 && desktop.at("requested_y") == 9,
        "The audit record did not carry the pointer geometry.");

    // Typed text is auditable by length only, so an audit trail cannot become a
    // keystroke log of everything Revia was asked to enter.
    auto typing = runtime.ParseJson(
        R"({"action":"type_text","application":"notepad.exe","value":"secret text"})");
    Check(typing.succeeded, "A type_text proposal did not parse.");
    typing.request.dryRun = true;
    Check(runtime.Execute(typing.request, true).Succeeded(),
        "A dry-run typing action did not reach the executor.");
    std::ifstream reread(audit);
    std::string line;
    while (std::getline(reread, line))
    {
        Check(line.find("secret text") == std::string::npos,
            "Typed text was written into the action audit log.");
    }
}

} // namespace

void RunDesktopControlTests()
{
    TestDesktopControlIsOffUntilGranted();
    TestUnapprovedApplicationIsRefused();
    TestRawCoordinatesNeedTheirOwnPermission();
    TestRawCoordinatesCannotOutliveThePointer();
    TestARegeneratedRuntimeIdIsNotAMovedCaret();
    TestVisualTargetingIsItsOwnPermission();
    TestAVisualTargetMustBeGroundedInSomething();
    TestAStaleVisualTargetIsNotClicked();
    TestVisionCanProposeTheOrdinaryVocabulary();
    TestAutonomousDesktopWorkIsSeparatelyPermitted();
    TestKeyChordsCannotLeaveTheApplication();
    TestSwitchingChordsFollowTheScope();
    TestKeyChordNormalization();
    TestConfinedInputNeedsAnApplication();
    TestWholeDesktopNeedsItsPrerequisites();
    TestWholeDesktopAcceptsScreenSpaceInput();
    TestCommandSurfacesStayOutOfReach();
    TestThePlannerAndParserShareOneVocabulary();
    TestConsequenceIsReadFromTheTargetNotTheVerb();
    TestTheConsequenceCeilingLoadsAndDefaultsNarrow();
    TestTheCeilingOnlyEverAddsRefusals();
    TestTypedTextIsBounded();
    TestLaunchArgumentStaysInsideApprovedRoots();
    TestOwnerFullAccessRaisesOnlyTheCeiling();
    TestStopGuardLatchesUntilResumed();
    TestExecutorRefusesWithoutAStopPath();
    TestInputBudgetIsSeparateFromUiAutomation();
    TestCommandsAndJsonParseIntoTypedRequests();
    TestRuntimeRegistersAndReportsDesktopControl();
    TestDesktopCommandTravelsTheWholePipeline();
    std::cout << "Desktop control tests passed.\n";
}
