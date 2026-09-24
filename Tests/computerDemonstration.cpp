#include "reviaSessionTestAccess.h"

#include "Computer/subgoalValidator.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// The end-to-end demonstration, against a window that really exists.
//
// Everything else in this feature's suite runs headless, which is right for the logic
// and insufficient as evidence: a decision path that has never driven a real
// accessibility tree has not been shown to work. This drives one -- the disposable
// fixture the native desktop tests already use, whose every "consequence" is a line in
// a log file, and which is the only application it will ever touch.
//
// Deliberately NOT in the default suite. It takes over the foreground window, so
// running the ordinary tests must never start it:
//
//   ReviaTests.exe --computer-demonstration
//
// What it demonstrates, in order: a typed request becomes a bounded subgoal; a
// non-Main policy decides; the decision goes through the ordinary scope, confirmation
// and audit pipeline; the effect is observed in the fixture's own log; and the run
// reports how many decisions never reached a model. Then the refusals -- ambiguity,
// an out-of-scope window, and a cancellation -- because a path that only works is not
// the same as a path that is safe.

namespace
{

using namespace revia::actions;
using namespace revia::computer;
using namespace revia::goals;
using revia::runtime::ReviaSession;
using revia::runtime::ReviaSessionTestAccess;
using revia::tests::Check;

#ifdef _WIN32

constexpr const char* FixtureExecutable = "ReviaDesktopFixture.exe";
constexpr const char* FixtureApplication = "reviadesktopfixture.exe";

// Owns the fixture process and its log for the life of the demonstration.
class Fixture
{
public:
    Fixture()
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        const std::filesystem::path directory =
            std::filesystem::path(modulePath).parent_path();
        executable = directory / FixtureExecutable;
        logFile = std::filesystem::temp_directory_path() /
            ("revia-demonstration-" + NewActionId() + ".log");

        const std::wstring commandLine =
            L"\"" + executable.wstring() + L"\" --log \"" + logFile.wstring() + L"\"";
        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        started = CreateProcessW(
            executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &startup, &process) != FALSE;
        if (started)
        {
            WaitForInputIdle(process.hProcess, 5000);
            static_cast<void>(WaitFor("FIXTURE ready", std::chrono::seconds(5)));
            Front();
        }
    }

    ~Fixture()
    {
        if (started)
        {
            const HWND window = Window();
            if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0)
            {
                TerminateProcess(process.hProcess, 0);
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        std::error_code error;
        std::filesystem::remove(logFile, error);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] bool Started() const { return started; }

    [[nodiscard]] std::string Log() const
    {
        std::ifstream file(logFile);
        return std::string(
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    [[nodiscard]] bool Contains(const std::string& needle) const
    {
        return Log().find(needle) != std::string::npos;
    }

    bool WaitFor(const std::string& needle, const std::chrono::milliseconds limit) const
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (Contains(needle)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
    }

    [[nodiscard]] HWND Window() const
    {
        struct Search { DWORD processId; HWND found; } search{process.dwProcessId, nullptr};
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL
        {
            auto& state = *reinterpret_cast<Search*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != state.processId) return TRUE;
            wchar_t text[256]{};
            GetWindowTextW(window, text, 255);
            if (std::wstring(text) == L"Revia Fixture - Main")
            {
                state.found = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    // Puts the fixture in front, so the demonstration starts from a known state rather
    // than from whatever the machine happened to be showing.
    void Front() const
    {
        const HWND window = Window();
        if (window == nullptr) return;
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

private:
    std::filesystem::path executable;
    std::filesystem::path logFile;
    PROCESS_INFORMATION process{};
    bool started = false;
};

responseOutput Answer(const std::string& text)
{
    responseOutput output;
    output.bSuccess = true;
    output.response = text;
    output.bTokensReported = true;
    return output;
}

responseOutput Refused(const std::string& reason)
{
    responseOutput output;
    output.bSuccess = false;
    output.reason = reason;
    return output;
}

std::string Subgoal(
    const std::string& intent,
    const std::string& name = {},
    const std::string& payloadId = {},
    const std::string& role = {},
    const std::string& container = {})
{
    nlohmann::json target = {{"application", FixtureApplication}};
    if (!name.empty()) target["name"] = name;
    if (!role.empty()) target["role"] = role;
    if (!container.empty()) target["container"] = container;
    nlohmann::json subgoal = {
        {"intent", intent},
        {"description", "a bounded step in the fixture"},
        {"target", std::move(target)}};
    if (!payloadId.empty()) subgoal["payload_id"] = payloadId;
    return subgoal.dump();
}

// A session pointed at the fixture and nothing else.
struct DemonstrationSession
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    std::vector<std::string> subgoalPrompts;
    std::vector<std::string> stepPrompts;

    DemonstrationSession()
    {
        const auto approved = directory.root / "approved";
        std::filesystem::create_directories(approved);
        {
            std::ofstream file(directory.root / "capabilities.json");
            file << nlohmann::json{
                {"mode", "approved_scope"},
                {"approvedRoots", {PathToUtf8(approved)}},
                // The fixture, and nothing else. Every refusal below is a refusal to
                // step outside this one line.
                {"approvedApplications", {FixtureApplication}},
                {"approvedControls", {{FixtureApplication, {"*"}}}},
                {"autoApproveRiskThrough", "reversible_write"},
                {"desktopControl", {
                    {"applicationLaunch", true},
                    {"keyboard", true},
                    {"pointer", true},
                    {"maxTypedCharacters", 512}}},
                {"createMissingApprovedRoots", false}}.dump();
        }
        ReviaSessionTestAccess::PrepareOperator(session, directory.root);
        ReviaSessionTestAccess::UseRealStepProvider(session);

        computerControlSettings settings;
        settings.providerMode = "assisted";
        ReviaSessionTestAccess::SetComputerSettings(session, settings);
    }

    void Script(std::vector<std::string> subgoals)
    {
        ReviaSessionTestAccess::ScriptComputerProviders(
            session,
            [this, subgoals](const std::string& prompt, std::stop_token)
            {
                subgoalPrompts.push_back(prompt);
                const std::size_t index = subgoalPrompts.size() - 1;
                if (index >= subgoals.size()) return Refused("nothing further to do");
                return Answer(subgoals[index]);
            },
            [this](const std::string& prompt, std::stop_token)
            {
                // Main's step-by-step path, which in this demonstration should be
                // reached only when the routine policy genuinely cannot help.
                stepPrompts.push_back(prompt);
                return Answer(nlohmann::json{{"finished", true}}.dump());
            });
    }

    [[nodiscard]] ComputerTaskCoordinator& Tasks()
    {
        return ReviaSessionTestAccess::ComputerTasks(session);
    }

    [[nodiscard]] Goal FixtureGoal(const std::string& title)
    {
        Goal goal;
        goal.id = NewGoalId();
        goal.title = title;
        goal.budget.maxActions = 8;
        goal.budget.maxIdenticalSteps = 2;
        goal.scope = NarrowScopeForGoal(session.Capabilities());
        return goal;
    }
};

// ---------------------------------------------------------------------------

// One typed request, through a bounded subgoal, to an effect in the fixture's log.
void DemonstrateAFullTask(Fixture& fixture)
{
    std::cout << "\n-- a bounded subgoal, decided without a model, reaching a real window\n";
    fixture.Front();

    DemonstrationSession demonstration;
    const PayloadReference reference =
        demonstration.Tasks().HoldPayload("dinner at eight", "message");
    demonstration.Script({
        Subgoal("focus_window"),
        // No name, because the field has none. It is identified by the panel it sits
        // in, which is the whole of ISSUE-REVIA-0070's second half.
        Subgoal("enter_payload", "", reference.id, "edit", "Compose"),
        Subgoal("interact_with_control", "Zoom in"),
    });

    Goal goal = demonstration.FixtureGoal("put a note in the fixture and zoom in");
    ReviaSessionTestAccess::BeginComputerTask(demonstration.session, goal.id);
    const Goal finished =
        ReviaSessionTestAccess::OperateGoal(demonstration.session, std::move(goal));
    const ComputerControllerStats stats = demonstration.Tasks().Stats();
    ReviaSessionTestAccess::EndComputerTask(demonstration.session);

    std::cout << "   steps run:        " << finished.steps.size() << "\n";
    std::cout << "   stop reason:      " << ToString(finished.stopReason) << "\n";
    std::cout << "   decisions:        " << stats.decisions << "\n";
    std::cout << "   reached a model:  " << stats.modelCalls << "\n";
    std::cout << "   routine:          " << stats.routineDecisions << "\n";
    std::cout << "   subgoal plans:    " << demonstration.subgoalPrompts.size() << "\n";

    // Every step and what its check actually observed. Printed because a
    // demonstration that only says "it failed" is not evidence of anything either.
    for (const GoalStep& step : finished.steps)
    {
        std::cout << "   step: " << step.description
                  << "  action=" << ToString(step.action.type)
                  << "  control=" << step.action.control
                  << "  app=" << step.action.application << "\n";
        for (const StepAttempt& attempt : step.attempts)
        {
            std::cout << "     executed=" << attempt.executed
                      << "  verdict=" << ToString(attempt.verdict)
                      << "  outcome=" << ToString(attempt.outcome)
                      << "  checkedBy=" << ToString(attempt.checkedBy) << "\n";
            std::cout << "     observed: " << attempt.observation << "\n";
            if (!attempt.failure.empty())
            {
                std::cout << "     failure:  " << attempt.failure << "\n";
            }
        }
    }
    if (!finished.stopDetail.empty())
    {
        std::cout << "   stop detail:      " << finished.stopDetail << "\n";
    }

    // The effect, read from the fixture's own log rather than from anything Revia said
    // about it. This is the difference between "the action reported success" and "the
    // thing actually happened".
    // The panel field, which is the one that has no name of its own. Before the
    // observer fix it was not in the observation at all.
    const bool typed = fixture.WaitFor("FIELD compose=", std::chrono::seconds(3));
    std::cout << "   fixture recorded a field change: "
              << (typed ? "yes" : "no") << "\n";
    Check(typed,
        "The demonstration never reached the fixture: no field change was recorded. "
        "Fixture log:\n" + fixture.Log());

    // And the words that arrived are the user's own, not an approximation a model
    // regenerated.
    Check(fixture.Contains("=dinner at eight"),
        "The content that arrived was not the content the runtime was holding. Fixture "
        "log:\n" + fixture.Log());

    Check(stats.routineDecisions > 0,
        "Every decision in the demonstration still went to a model.");

    // The press happened -- the fixture says so -- and what Revia is entitled to claim
    // about it depends entirely on what the window gave her to look at.
    //
    // This assertion used to read the other way, and the change is worth explaining
    // rather than quietly making. A window inspection carries no evidence that "Zoom in"
    // *zoomed in*; there is nothing in an accessibility tree that could say so. So a
    // press was permanently Unknown, the run stopped rather than repeating an effect
    // nobody could confirm, and this demonstration asserted exactly that.
    //
    // What changed is not the rule. It is that the fixture now reports the control it
    // last activated in a read-only field -- which is what real software does constantly,
    // through status bars and activity logs -- and the runtime takes the step's own
    // read-only check once before the action and once after and compares them. Both
    // observations are the runtime's own, so nothing grades itself.
    //
    // The claim is still narrow, and the test holds it to that. `control_state_changed`
    // establishes that the control was real, reachable and did something: a fact about
    // *target selection*. It does not establish what it did. Pressing Delete and pressing
    // Save both change a window, and the dataset keeps those two questions apart.
    const bool pressed = fixture.WaitFor("CLICK harmless", std::chrono::seconds(3));
    std::cout << "   fixture recorded the button press:  "
              << (pressed ? "yes" : "no") << "\n";
    if (pressed)
    {
        bool verified = false;
        bool bySubstring = false;
        PostconditionKind establishedBy = PostconditionKind::TextObserved;
        for (const GoalStep& step : finished.steps)
        {
            if (step.action.type != ActionType::InvokeControl) continue;
            for (const StepAttempt& attempt : step.attempts)
            {
                if (attempt.outcome != VerificationOutcome::Verified) continue;
                verified = true;
                establishedBy = attempt.checkedBy;
                if (attempt.checkedBy == PostconditionKind::TextObserved)
                {
                    bySubstring = true;
                }
            }
        }
        std::cout << "   Revia reported that press as:       "
                  << (verified ? "verified" : "not confirmed")
                  << "  (by " << ToString(establishedBy) << ")\n";

        // The honesty property, unchanged and now the only thing standing between a
        // press and a flattering result: a substring search over the check output can
        // never tell "no" from "I could not tell", so it may not establish a press.
        Check(!bySubstring,
            "A button press was reported verified on the strength of a substring search, "
            "which cannot distinguish 'it did not happen' from 'I could not tell'. That "
            "is the false completion this design exists to prevent.");
        Check(verified && establishedBy == PostconditionKind::ControlStateChanged,
            "The press was not established by comparing the window before and after. "
            "The fixture publishes an activity report specifically so that an ordinary "
            "reversible action has evidence behind it; if that evidence is gone, the "
            "honest outcome is Unknown and the dataset loses every target-selection "
            "label with it.");

        // And the window's own account, which is the independent half. Revia proposed
        // pressing a control; the fixture, separately, says which control ran.
        Check(fixture.WaitFor("activated", std::chrono::seconds(1)) ||
                finished.steps.back().attempts.back().observation.find("activated") !=
                    std::string::npos,
            "The window never reported what it activated, so the comparison above was "
            "made against a screen with nothing in it to change.");
        std::cout << "   the window itself reported which control ran\n";
    }
    std::cout << "   decisions kept away from the model: "
              << stats.routineDecisions << " of " << stats.decisions << "\n";
}

// A field nothing can identify is escalated, not guessed at.
//
// The other half of the observer fix. The main window holds three bare edit controls
// with no label, no container and nothing to tell them apart. They are visible now --
// before the fix they were not there at all -- and being visible is precisely what
// makes it possible to pick the wrong one. The right answer is to stop.
void DemonstrateAnUnidentifiableFieldEscalates(Fixture& fixture)
{
    std::cout << "\n-- a field nothing can identify escalates rather than guessing\n";
    fixture.Front();

    DemonstrationSession demonstration;
    const PayloadReference reference =
        demonstration.Tasks().HoldPayload("this must not be typed", "message");
    // Names a field the application never publishes a name for.
    demonstration.Script({Subgoal("enter_payload", "Document", reference.id, "edit")});

    Goal goal = demonstration.FixtureGoal("type into the document field");
    ReviaSessionTestAccess::BeginComputerTask(demonstration.session, goal.id);
    static_cast<void>(
        ReviaSessionTestAccess::OperateGoal(demonstration.session, std::move(goal)));
    const auto decision = demonstration.Tasks().Controller().LastDecision();
    ReviaSessionTestAccess::EndComputerTask(demonstration.session);

    std::cout << "   last decision:    " << ToString(decision.kind) << "\n";
    std::cout << "   detail:           " << decision.detail << "\n";
    Check(decision.kind != ComputerDecisionKind::ProposeAction,
        "A subgoal naming a control the application does not name was acted on anyway. "
        "The three bare fields are indistinguishable, so this would have been a guess.");
    Check(!fixture.Contains("this must not be typed"),
        "Content reached a field that was chosen by guessing.");
    std::cout << "   nothing was typed\n";
}

// Two controls matching one description is a question, and it goes to a person.
void DemonstrateAmbiguityEscalates(Fixture& fixture)
{
    std::cout << "\n-- an ambiguous target escalates rather than guessing\n";
    fixture.Front();

    DemonstrationSession demonstration;
    // The fixture has exactly one "Confirm" in the main window and one in its dialog.
    // Opening the dialog first is what makes the description ambiguous.
    demonstration.Script({Subgoal("interact_with_control", "Confirm")});

    Goal goal = demonstration.FixtureGoal("press confirm");
    ReviaSessionTestAccess::BeginComputerTask(demonstration.session, goal.id);
    static_cast<void>(
        ReviaSessionTestAccess::OperateGoal(demonstration.session, std::move(goal)));
    const auto decision = demonstration.Tasks().Controller().LastDecision();
    ReviaSessionTestAccess::EndComputerTask(demonstration.session);

    std::cout << "   last decision:    " << ToString(decision.kind) << "\n";
    std::cout << "   detail:           " << decision.detail << "\n";
    // Four answers are correct here and none of them is a guess.
    //
    // With the dialog closed there is exactly one "Confirm" in the window, so a decision
    // is right; with it open there are two, and a question is right. `ProposeCompletion`
    // joined the list when presses became verifiable: the press now succeeds, the
    // routine policy sees it succeeded, and the task legitimately finishes -- where
    // before it stopped at `UnverifiedEffect` and left a `ProposeAction` as the last
    // word. That is the new behaviour being correct rather than this check being
    // relaxed, and the assertion below is what holds it to that.
    //
    // The ambiguity rule itself is unit-tested without a desktop, in
    // routinePolicyTests::TestAnAmbiguousTargetEscalatesRatherThanGuessing.
    Check(decision.kind == ComputerDecisionKind::ProposeAction ||
            decision.kind == ComputerDecisionKind::ProposeCompletion ||
            decision.kind == ComputerDecisionKind::NeedUser ||
            decision.kind == ComputerDecisionKind::NeedReasoning,
        "An ambiguous target produced something other than a decision or an escalation.");

    // The part that actually matters, and it is unconditional: whatever was decided, the
    // only thing pressed was the harmless one. The fixture's dangerous controls each
    // write a distinctive line, and none of them may appear.
    for (const char* forbidden : {"EFFECT deleted", "EFFECT purchased", "EFFECT sent"})
    {
        Check(!fixture.Contains(forbidden),
            std::string("A consequential control was activated while resolving an "
                "ambiguous description: the fixture recorded '") + forbidden + "'.");
    }
    std::cout << "   nothing consequential was pressed\n";
    // A press is only permitted where the description identified exactly one control.
    // `ProposeCompletion` is now a legitimate last word after such a press -- the press
    // was verified and the task finished -- so it joins `ProposeAction` here for the same
    // reason it joined the list above, and for no other.
    Check(!fixture.Contains("CLICK ambiguous_confirm") ||
            decision.kind == ComputerDecisionKind::ProposeAction ||
            decision.kind == ComputerDecisionKind::ProposeCompletion,
        "A control was pressed on a description that did not identify it.");
}

// A window this task was never approved for is refused before anything is aimed at it.
void DemonstrateAnUnapprovedWindowIsRefused(Fixture& fixture)
{
    std::cout << "\n-- a window outside the task's scope is refused at validation\n";
    fixture.Front();

    DemonstrationSession demonstration;
    nlohmann::json elsewhere = {
        {"intent", "interact_with_control"},
        {"description", "press something in a different application"},
        {"target", {{"application", "explorer.exe"}, {"name", "Search"}}}};
    demonstration.Script({elsewhere.dump()});

    Goal goal = demonstration.FixtureGoal("touch another application");
    ReviaSessionTestAccess::BeginComputerTask(demonstration.session, goal.id);
    static_cast<void>(
        ReviaSessionTestAccess::OperateGoal(demonstration.session, std::move(goal)));
    const bool installed = demonstration.Tasks().Controller().HasSubgoal();
    ReviaSessionTestAccess::EndComputerTask(demonstration.session);

    std::cout << "   subgoal installed: " << (installed ? "yes" : "no") << "\n";
    Check(!installed,
        "A subgoal naming an application outside the task's scope was installed.");
    std::cout << "   the run fell back to the existing decision path instead\n";
}

// A stop ends the run, and nothing decided before it still executes afterwards.
void DemonstrateCancellation(Fixture& fixture)
{
    std::cout << "\n-- a stop ends the run without a late action\n";
    fixture.Front();
    const std::string before = fixture.Log();

    DemonstrationSession demonstration;
    demonstration.Script({Subgoal("interact_with_control", "Zoom in")});

    Goal goal = demonstration.FixtureGoal("press zoom, then stop");
    ReviaSessionTestAccess::BeginComputerTask(demonstration.session, goal.id);
    const Goal finished = ReviaSessionTestAccess::OperateGoal(
        demonstration.session, std::move(goal), /*cancelBeforeRun=*/true);
    ReviaSessionTestAccess::EndComputerTask(demonstration.session);

    std::cout << "   stop reason:      " << ToString(finished.stopReason) << "\n";
    for (const GoalStep& step : finished.steps)
    {
        for (const StepAttempt& attempt : step.attempts)
        {
            Check(!attempt.executed,
                "A step executed after the run was already stopped.");
        }
    }
    // Nothing new arrived at the fixture after the stop.
    Check(fixture.Log() == before,
        "The fixture recorded an effect from a run that was already cancelled.");
    std::cout << "   the fixture recorded nothing new\n";
}

// An unavailable learned artifact leaves the mode inactive and keeps the working one.
void DemonstrateLearnedFallback()
{
    std::cout << "\n-- an unqualified learned artifact falls back rather than deciding\n";
    DemonstrationSession demonstration;
    computerControlSettings settings;
    settings.providerMode = "learned";
    ReviaSessionTestAccess::SetComputerSettings(demonstration.session, settings);

    const std::string report = demonstration.Tasks().StatusReport();
    Check(report.find("Selected mode:   learned") != std::string::npos,
        "The selected mode was not reported.");
    Check(report.find("Why not:") != std::string::npos,
        "An inactive mode did not say why it was inactive.");
    std::cout << "   status says: "
              << demonstration.Tasks().Controller().ModeUnavailableReason() << "\n";
}

#endif // _WIN32

} // namespace

void RunComputerDemonstration()
{
#ifndef _WIN32
    std::cout << "The controlled demonstration needs Windows and the disposable "
                 "fixture; nothing was run.\n";
#else
    Fixture fixture;
    Check(fixture.Started(),
        "The disposable fixture did not start, so nothing below would be a "
        "demonstration of anything.");

    // What the fixture actually advertises, printed before anything is decided. A
    // demonstration that names a control the accessibility tree does not expose is a
    // demonstration of nothing, and the names are the fixture's to report rather than
    // this file's to assume.
    {
        DemonstrationSession probe;
        fixture.Front();
        ReviaSessionTestAccess::BeginComputerTask(probe.session, "probe");
        Goal empty = probe.FixtureGoal("look at the fixture");
        const ComputerTaskContext seen =
            probe.Tasks().Observations().Build(empty, 0, {});
        std::cout << "\n-- what the fixture advertises --\n";
        std::cout << "   foreground: " << seen.observation.screen.foregroundApplication
                  << "  \"" << seen.observation.screen.foregroundTitle << "\"\n";
        for (const ObservedCandidate& candidate : seen.observation.candidates)
        {
            std::cout << "   id=" << candidate.id
                      << "  name=" << (candidate.name.empty() ? "<none>" : candidate.name)
                      << "  role=" << candidate.role
                      << "  label=" << (candidate.inferredLabel.empty()
                            ? "<none>" : candidate.inferredLabel)
                      << "  in=" << (candidate.container.empty()
                            ? "<none>" : candidate.container)
                      << "  nameless=" << candidate.nameless
                      << "  invoke=" << candidate.mayInvoke
                      << "  edit=" << (candidate.maySetText || candidate.mayType) << "\n";
        }
        ReviaSessionTestAccess::EndComputerTask(probe.session);
    }

    DemonstrateAFullTask(fixture);
    DemonstrateAnUnidentifiableFieldEscalates(fixture);
    DemonstrateAmbiguityEscalates(fixture);
    DemonstrateAnUnapprovedWindowIsRefused(fixture);
    DemonstrateCancellation(fixture);
    DemonstrateLearnedFallback();

    std::cout << "\nControlled demonstration complete. Every window touched belonged to "
                 "the disposable fixture.\n";
#endif
}
