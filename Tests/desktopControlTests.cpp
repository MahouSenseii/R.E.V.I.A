#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/desktopActionRateLimiter.h"
#include "Policy/desktopInputGuard.h"
#include "Policy/permissionStore.h"
#include "Windows/desktopControlExecutor.h"

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
        const bool autonomous = false)
    {
        settings["desktopControl"] = {
            {"pointer", pointer},
            {"keyboard", keyboard},
            {"applicationLaunch", launch},
            {"rawCoordinates", raw},
            {"autonomous", autonomous}};
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
        pointed.reason.find("raw coordinates is disabled") != std::string::npos,
        "A raw coordinate was accepted without the raw-coordinate permission.");

    // A vision-resolved element needs no raw-coordinate permission: the executor
    // re-finds the element rather than trusting the coordinate.
    ActionRequest resolved = PointerRequest();
    resolved.input.hasPoint = false;
    resolved.resolution.visionResolved = true;
    resolved.resolution.resolvedRuntimeId = "42.7";
    Check(policy.Evaluate(resolved).verdict == PolicyVerdict::RequiresConfirmation,
        "A vision-resolved click was not offered for confirmation.");

    ActionRequest aimless = PointerRequest();
    aimless.input.hasPoint = false;
    Check(policy.Evaluate(aimless).verdict == PolicyVerdict::Blocked,
        "A pointer action with no target at all was not blocked.");
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
    // The Windows key is not a supported modifier, so the run box, the start menu, and
    // search -- every keystroke route to a shell -- are unreachable by construction.
    for (const std::string chord : {"win+r", "win+s", "win+x", "win+l", "meta+e"})
    {
        const auto decision = policy.Evaluate(KeyRequest(chord));
        Check(decision.verdict == PolicyVerdict::Blocked,
            "A Windows-key chord was accepted: " + chord);
    }
    for (const std::string chord : {"alt+tab", "ctrl+shift+esc", "ctrl+escape",
             "ctrl+alt+delete", "alt+escape"})
    {
        const auto decision = policy.Evaluate(KeyRequest(chord));
        Check(decision.verdict == PolicyVerdict::Blocked &&
            decision.reason.find("switches away") != std::string::npos,
            "An application-switching chord was accepted: " + chord);
    }
    for (const std::string chord : {"", "ctrl", "ctrl+", "ctrl+s+a", "ctrl+notakey"})
    {
        Check(policy.Evaluate(KeyRequest(chord)).verdict == PolicyVerdict::Blocked,
            "A malformed chord was accepted: '" + chord + "'");
    }
    Check(policy.Evaluate(KeyRequest("Ctrl + Shift+S")).verdict ==
        PolicyVerdict::RequiresConfirmation,
        "A well-formed chord was not offered for confirmation.");
}

void TestKeyChordNormalization()
{
    KeyChord chord;
    std::string error;
    Check(ParseKeyChord("CTRL + shift + S", chord, error) &&
        chord.normalized == "ctrl+shift+s" && chord.modifierVirtualKeys.size() == 2 &&
        chord.virtualKey == 0x53,
        "A mixed-case chord did not normalize to ctrl+shift+s.");
    // Aliases collapse so the blocklist cannot be sidestepped by spelling.
    Check(ParseKeyChord("alt+esc", chord, error) == false &&
        error.find("switches away") != std::string::npos,
        "alt+esc was not recognized as the blocked alt+escape.");
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

    // Rejections. A malformed command must not become a request with defaults.
    for (const std::string command : {
             "/click \"notepad.exe\" \"Untitled\" \"x\" \"4\"",
             "/click \"notepad.exe\" \"Untitled\"",
             "/scroll \"notepad.exe\" \"Untitled\"",
             "/press \"notepad.exe\"",
             "/launch"})
    {
        const auto rejected = parser.ParseCommand(command);
        Check(rejected.recognized && !rejected.succeeded,
            "A malformed desktop command was accepted: " + command);
    }
    const auto missingApplication = parser.ParseJson(
        R"({"action":"click_pointer","x":1,"y":2})");
    Check(missingApplication.recognized && !missingApplication.succeeded,
        "A desktop proposal without an application was accepted.");
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

    // Persisted permission changes survive the reload and drop their subset authorities.
    Check(runtime.SetDesktopControl(false, true, false, true, true, error),
        "Desktop control settings could not be written: " + error);
    const auto reloaded = runtime.Settings().desktopControl;
    Check(!reloaded.pointer && reloaded.keyboard && !reloaded.rawCoordinates &&
        reloaded.autonomous,
        "Withdrawing pointer control did not withdraw raw coordinates with it.");
    Check(runtime.SetDesktopControl(false, false, false, false, true, error) &&
        !runtime.Settings().desktopControl.autonomous,
        "Autonomy survived the withdrawal of every desktop capability.");

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
    TestAutonomousDesktopWorkIsSeparatelyPermitted();
    TestKeyChordsCannotLeaveTheApplication();
    TestKeyChordNormalization();
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
