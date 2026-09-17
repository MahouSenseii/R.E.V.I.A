#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Goals/goalRunner.h"
#include "Goals/goalStore.h"
#include "Planning/goalPlanner.h"
#include "Windows/desktopObserver.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace
{

using namespace revia::goals;
using revia::tests::Check;

// The iterative loop, exercised against the filesystem rather than the desktop.
//
// The point of these tests is the control flow, not the actions: whether the loop stops
// when it should, refuses what it should, and records what actually happened rather than
// what was intended. Filesystem actions make that deterministic and mean the suite needs
// no window, no UI Automation, and no machine to drive.

nlohmann::json CapabilityConfig(const std::filesystem::path& approvedRoot)
{
    return {
        {"mode", "approved_scope"},
        {"approvedRoots", {revia::actions::PathToUtf8(approvedRoot)}},
        {"autoApproveRiskThrough", "reversible_write"},
        {"createMissingApprovedRoots", false}};
}

struct LoopFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path approved = directory.root / "sandbox";
    revia::actions::ActionRuntime runtime;
    GoalStore store{(directory.root / "goals.db").string()};
    GoalRunner runner{runtime, store};

    LoopFixture()
    {
        std::filesystem::create_directories(approved);
        const auto configPath = directory.root / "capabilities.json";
        {
            std::ofstream file(configPath);
            file << CapabilityConfig(approved).dump();
            Check(file.good(), "Could not write the loop fixture policy.");
        }
        std::string error;
        Check(runtime.Initialize(configPath, directory.root / "audit.jsonl", error),
            "The action runtime did not initialize for the loop: " + error);
    }

    [[nodiscard]] Goal NewGoal(const std::string& title) const
    {
        Goal goal;
        goal.title = title;
        goal.scope = NarrowScopeForGoal(runtime.Settings());
        return goal;
    }

    // One directory created, verified by listing its parent and looking for the name.
    // The check is read-only, which is what the runner insists on.
    [[nodiscard]] GoalStep MakeDirectory(const std::string& name) const
    {
        GoalStep step;
        step.description = "Create " + name;
        step.action.type = revia::actions::ActionType::CreateDirectory;
        step.action.source = approved / name;
        step.check.type = revia::actions::ActionType::ListDirectory;
        step.check.source = approved;
        step.expected = name;
        return step;
    }
};

NextStep Take(GoalStep step)
{
    NextStep next;
    next.hasStep = true;
    next.step = std::move(step);
    return next;
}

NextStep Done(const std::string& reason)
{
    NextStep next;
    next.finished = true;
    next.reason = reason;
    return next;
}

NextStep Stuck(const std::string& reason)
{
    NextStep next;
    next.reason = reason;
    return next;
}

void TestTheRunIsDiscoveredRatherThanPlanned()
{
    LoopFixture fixture;
    // Nothing is decided up front. The provider is asked again after every step, which
    // is the whole difference between this and the planned runner.
    fixture.runner.SetStepProvider([&](const Goal& goal, const std::uint32_t iteration)
    {
        Check(goal.steps.size() == iteration,
            "The provider was not shown every completed step.");
        if (iteration == 0) return Take(fixture.MakeDirectory("first"));
        if (iteration == 1) return Take(fixture.MakeDirectory("second"));
        return Done("both folders exist");
    });

    Goal goal = fixture.NewGoal("Make two folders");
    Check(goal.steps.empty(), "An iterative goal should start with no steps.");

    const Goal finished = fixture.runner.Operate(goal);
    Check(finished.status == GoalStatus::Succeeded &&
        finished.stopReason == StopReason::Completed,
        "A completable iterative goal did not complete.");
    Check(std::filesystem::is_directory(fixture.approved / "first") &&
        std::filesystem::is_directory(fixture.approved / "second"),
        "The loop reported success without doing the work.");
    Check(finished.steps.size() == 2 && finished.steps[0].ordinal == 0 &&
        finished.steps[1].ordinal == 1,
        "The steps actually taken were not recorded in order.");
    Check(finished.steps[0].attempts.size() == 1 &&
        finished.steps[0].attempts.front().verified &&
        finished.steps[1].attempts.front().verified,
        "The loop advanced without recording verification evidence.");
    // Two actions per step: doing it, then looking to see whether it happened.
    Check(finished.spend.actions == 4,
        "A two-step run did not cost two actions and two observations.");
    Check(fixture.store.Load(finished.id).has_value(),
        "The finished iterative goal was not persisted.");
}

void TestStuckIsNotTheSameAsFinished()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        if (iteration == 0) return Take(fixture.MakeDirectory("only"));
        return Stuck("I cannot tell what to do next");
    });

    const Goal finished = fixture.runner.Operate(fixture.NewGoal("Get stuck"));
    Check(finished.status == GoalStatus::Blocked &&
        finished.stopReason == StopReason::Undecided,
        "Running out of ideas was not distinguished from finishing.");
    Check(finished.stopDetail == "I cannot tell what to do next" &&
        fixture.store.Load(finished.id)->stopDetail == finished.stopDetail,
        "The planner's stopping explanation was lost during execution or persistence.");
    // The work it did manage still stands and is still recorded.
    Check(std::filesystem::is_directory(fixture.approved / "only") &&
        finished.steps.size() == 1 && finished.steps.front().attempts.front().verified,
        "Stopping undecided discarded the step that did succeed.");
}

void TestOldGoalDatabaseRetainsItsRecords()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = (directory.root / "old-goals.db").string();
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Legacy store could not open.");
    const int result = sqlite3_exec(database,
        "CREATE TABLE goals (id TEXT PRIMARY KEY, title TEXT NOT NULL, status TEXT NOT NULL,"
        "stop_reason TEXT NOT NULL, current_step INTEGER NOT NULL DEFAULT 0, budget TEXT NOT NULL,"
        "spend TEXT NOT NULL, scope TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);"
        "INSERT INTO goals VALUES ('old','Existing goal','blocked','undecided',0,'{}','{}','{}','1','1');",
        nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(result == SQLITE_OK, "Legacy store fixture could not be created.");
    GoalStore store(path);
    auto old = store.Load("old");
    Check(old && old->title == "Existing goal" && old->status == GoalStatus::Blocked &&
        old->stopDetail.empty(), "Adding goal explanations changed a legacy record.");
    old->stopDetail = "New diagnostic";
    Check(store.Save(*old) && store.Load("old")->stopDetail == "New diagnostic",
        "An upgraded store could not retain a diagnostic.");
}

void TestStopDuringPlanningWinsOverLateCompletion()
{
    LoopFixture fixture;
    std::stop_source stop;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        stop.request_stop();
        return Done("late model answer");
    });
    const auto result = fixture.runner.Operate(fixture.NewGoal("Stop during planning"), stop.get_token());
    Check(result.status == GoalStatus::Cancelled && result.spend.actions == 0,
        "A late planner answer overrode cancellation.");
}

void TestARunWithNoProviderRefuses()
{
    LoopFixture fixture;
    const Goal finished = fixture.runner.Operate(fixture.NewGoal("No provider"));
    Check(finished.status == GoalStatus::Failed &&
        finished.stopReason == StopReason::InvalidPlan && finished.steps.empty(),
        "A loop with nothing to ask reported anything other than a refusal.");
}

void TestRepeatingTheSameActionStopsTheRun()
{
    LoopFixture fixture;
    std::uint32_t asked = 0;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        ++asked;
        // Always the identical action. It even succeeds and verifies every time, which
        // is exactly the shape a budget alone would happily run to exhaustion.
        return Take(fixture.MakeDirectory("same"));
    });

    Goal goal = fixture.NewGoal("Loop forever");
    goal.budget.maxIdenticalSteps = 2;
    goal.budget.maxActions = 100;

    const Goal finished = fixture.runner.Operate(goal);
    Check(finished.status == GoalStatus::Blocked &&
        finished.stopReason == StopReason::NoProgress,
        "Choosing the same action over and over did not stop the run.");
    // Two ran, the third was refused before it executed rather than after.
    Check(finished.steps.size() == 2 && asked == 3,
        "The guard did not stop on the choice that would have been pointless: " +
            std::to_string(finished.steps.size()) + " steps, " +
            std::to_string(asked) + " asks.");
    Check(finished.spend.actions < goal.budget.maxActions,
        "The stuck run consumed its whole action budget.");
}

void TestDifferentTargetsCountAsProgress()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        // The same verb every time, a different target every time. That is work, not a
        // loop, and the guard has to tell them apart.
        if (iteration < 5) return Take(fixture.MakeDirectory("folder" + std::to_string(iteration)));
        return Done("five folders");
    });

    Goal goal = fixture.NewGoal("Five folders");
    goal.budget.maxIdenticalSteps = 2;
    goal.budget.maxActions = 40;

    const Goal finished = fixture.runner.Operate(goal);
    Check(finished.status == GoalStatus::Succeeded && finished.steps.size() == 5,
        "Repeating a verb against new targets was mistaken for being stuck.");
}

void TestAnUnverifiableStepIsRefusedMidRun()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        // A step that would change something and then "verify" by changing something
        // else. A plan containing this is rejected before it runs; a step invented
        // mid-run has to face the same check or the loop becomes the way around it.
        GoalStep step = fixture.MakeDirectory("unchecked");
        step.check.type = revia::actions::ActionType::CreateDirectory;
        step.check.source = fixture.approved / "sneaky";
        return Take(step);
    });

    const Goal finished = fixture.runner.Operate(fixture.NewGoal("Skip verification"));
    Check(finished.status == GoalStatus::Failed &&
        finished.stopReason == StopReason::InvalidPlan,
        "A step verifying with a mutating action was accepted mid-run.");
    Check(!std::filesystem::exists(fixture.approved / "unchecked") &&
        !std::filesystem::exists(fixture.approved / "sneaky"),
        "The refused step executed before it was refused.");

    LoopFixture missing;
    missing.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        GoalStep step = missing.MakeDirectory("nameless");
        step.expected.clear();
        return Take(step);
    });
    const Goal unsaid = missing.runner.Operate(missing.NewGoal("Say nothing"));
    Check(unsaid.status == GoalStatus::Failed &&
        unsaid.stopReason == StopReason::InvalidPlan &&
        !std::filesystem::exists(missing.approved / "nameless"),
        "A step that never says what success looks like was executed.");
}

void TestVerificationFailureStopsTheRun()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        // The action succeeds; the observation afterwards does not show what the step
        // claimed it would. Doing something is not evidence of having done the thing.
        GoalStep step = fixture.MakeDirectory("real");
        step.expected = "a-folder-that-was-never-created";
        return Take(step);
    });

    Goal goal = fixture.NewGoal("Claim more than happened");
    goal.budget.maxRetriesPerStep = 0;

    const Goal finished = fixture.runner.Operate(goal);
    Check(finished.status == GoalStatus::Failed,
        "An unverified step let the run continue.");
    Check(finished.steps.size() == 1 && !finished.steps.front().attempts.empty() &&
        !finished.steps.front().attempts.front().verified,
        "The failed verification was not recorded as evidence.");
    // The action really did happen, and the record says so rather than pretending the
    // whole step was a no-op.
    Check(std::filesystem::is_directory(fixture.approved / "real") &&
        finished.steps.front().attempts.front().executed,
        "The run hid that the action had already taken effect.");
}

void TestTheLoopCannotWidenItsScope()
{
    LoopFixture fixture;
    const std::filesystem::path outside = fixture.directory.root / "not-approved";
    std::filesystem::create_directories(outside);
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        GoalStep step;
        step.description = "Reach outside the sandbox";
        step.action.type = revia::actions::ActionType::CreateDirectory;
        step.action.source = outside / "escaped";
        step.check.type = revia::actions::ActionType::ListDirectory;
        step.check.source = outside;
        step.expected = "escaped";
        return Take(step);
    });

    const Goal finished = fixture.runner.Operate(fixture.NewGoal("Escape"));
    Check(finished.status == GoalStatus::Blocked &&
        finished.stopReason == StopReason::PolicyBlocked,
        "A step outside the goal's scope was not blocked.");
    Check(!std::filesystem::exists(outside / "escaped"),
        "A blocked step still reached the filesystem.");
}

void TestBudgetsAndCancellationStillApply()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        return Take(fixture.MakeDirectory("folder" + std::to_string(iteration)));
    });

    Goal bounded = fixture.NewGoal("Run past the budget");
    bounded.budget.maxActions = 5;
    bounded.budget.maxIdenticalSteps = 0;
    const Goal exhausted = fixture.runner.Operate(bounded);
    Check(exhausted.status == GoalStatus::Exhausted,
        "An endless provider was not stopped by the action budget.");
    Check(exhausted.spend.actions <= bounded.budget.maxActions,
        "The run spent more actions than its budget allowed.");

    std::stop_source stopSource;
    stopSource.request_stop();
    const Goal cancelled =
        fixture.runner.Operate(fixture.NewGoal("Cancelled"), stopSource.get_token());
    Check(cancelled.status == GoalStatus::Cancelled &&
        cancelled.stopReason == StopReason::Cancelled && cancelled.steps.empty(),
        "An already-cancelled loop still ran a step.");
}

void TestPlannedRunsAreUnchanged()
{
    // The planned path is a working pipeline and this task must not have moved it.
    LoopFixture fixture;
    Goal goal = fixture.NewGoal("A planned goal");
    goal.steps.push_back(fixture.MakeDirectory("planned"));

    const Goal finished = fixture.runner.Run(goal);
    Check(finished.status == GoalStatus::Succeeded &&
        finished.stopReason == StopReason::Completed &&
        std::filesystem::is_directory(fixture.approved / "planned"),
        "The planned goal runner regressed.");

    Goal empty = fixture.NewGoal("An empty plan");
    const Goal rejected = fixture.runner.Run(empty);
    Check(rejected.status == GoalStatus::Failed &&
        rejected.stopReason == StopReason::InvalidPlan,
        "An empty plan stopped being rejected.");
}

// The observation half. Reading a live window needs a live window, so what is tested
// here is everything that is a pure function of what was read: the digest the loop uses
// to tell acting from achieving, and the bounding that stops a hostile window from
// eating the decision budget.

revia::actions::windows::ObservedControl MakeControl(
    const std::string& name, const int x, const int y)
{
    revia::actions::windows::ObservedControl control;
    control.name = name;
    control.runtimeId = "42." + name;
    control.controlType = 50000;
    control.left = x;
    control.top = y;
    control.right = x + 80;
    control.bottom = y + 24;
    control.enabled = true;
    control.invokable = true;
    return control;
}

revia::actions::windows::DesktopObservation MakeObservation()
{
    revia::actions::windows::DesktopObservation observation;
    observation.succeeded = true;
    observation.foregroundApplication = "notepad.exe";
    observation.foregroundTitle = "Untitled - Notepad";
    observation.windowRight = 800;
    observation.windowBottom = 600;
    observation.controls.push_back(MakeControl("Save", 40, 10));
    observation.controls.push_back(MakeControl("Cancel", 140, 10));
    return observation;
}

void TestTheObservationDigestNoticesChange()
{
    const auto original = MakeObservation();
    Check(original.Fingerprint() == MakeObservation().Fingerprint(),
        "Two identical observations produced different digests.");

    // Each of these is something an action might have caused, and each has to register
    // as the world having changed.
    auto moved = MakeObservation();
    moved.controls[0].left += 3;
    Check(moved.Fingerprint() != original.Fingerprint(),
        "A control moving did not change the digest.");

    auto renamed = MakeObservation();
    renamed.foregroundTitle = "Notes - Notepad";
    Check(renamed.Fingerprint() != original.Fingerprint(),
        "The window title changing did not change the digest.");

    auto enabled = MakeObservation();
    enabled.controls[1].enabled = false;
    Check(enabled.Fingerprint() != original.Fingerprint(),
        "A control becoming disabled did not change the digest.");

    auto appeared = MakeObservation();
    appeared.controls.push_back(MakeControl("Don't Save", 240, 10));
    Check(appeared.Fingerprint() != original.Fingerprint(),
        "A new control appearing did not change the digest.");

    // A failed observation must never collide with a real one, or "I could not look"
    // would read as "nothing changed".
    revia::actions::windows::DesktopObservation blind;
    blind.failure = "No window currently has focus.";
    Check(blind.Fingerprint() != original.Fingerprint() &&
        blind.Fingerprint().find("unobserved") != std::string::npos,
        "A failed observation was not distinguishable from a real one.");
}

void TestVerificationEvidenceSurvivesTheHistoryLimit()
{
    LoopFixture fixture;
    for (int index = 0; index < 30; ++index)
        std::filesystem::create_directory(fixture.approved /
            ("aaa-unrelated-entry-" + std::to_string(index)));
    Goal goal = fixture.NewGoal("Create the requested directory");
    goal.steps.push_back(fixture.MakeDirectory("zzz-requested-result"));
    const Goal finished = fixture.runner.Run(std::move(goal));
    Check(finished.status == GoalStatus::Succeeded, "The evidence fixture did not finish.");
    const auto persisted = fixture.store.Load(finished.id);
    Check(persisted && persisted->steps[0].attempts[0].observation.substr(0, 400)
        .find("zzz-requested-result") != std::string::npos,
        "The evidence that verified the step was erased by the planner history limit.");
}

void TestExplicitPlannerDecisions()
{
    using revia::planning::GoalPlanner;
    const auto act = GoalPlanner::ParseNextStep(R"({"decision":"act","description":"Open Edge","step":{
        "action":{"action":"launch_application","application":"msedge.exe"},
        "check":{"action":"inspect_window","application":"msedge.exe"},
        "expected":"msedge.exe"}})");
    Check(act.succeeded && !act.finished &&
        act.step.action.type == revia::actions::ActionType::LaunchApplication,
        "An explicit act decision did not reach the ordinary step parser.");
    const auto complete = GoalPlanner::ParseNextStep(
        R"({"decision":"complete","reason":"The page title confirms success."})");
    const auto blocked = GoalPlanner::ParseNextStep(
        R"({"decision":"blocked","reason":"No permitted target is visible."})");
    Check(complete.succeeded && complete.finished && blocked.succeeded && !blocked.finished,
        "Explicit completion and blocking were collapsed.");
    for (const char* invalid : {R"({"decision":"act"})", R"({"decision":"other"})",
            R"({"decision":"complete"})", R"({"decision":42})"})
        Check(!GoalPlanner::ParseNextStep(invalid).succeeded,
            "An incomplete explicit decision was accepted.");
}

void TestDesktopWorkNeedsEvidenceFromItsOwnWindow()
{
    using revia::actions::ActionType;
    GoalStep step;
    step.action.type = ActionType::TypeText;
    step.action.application = "msedge.exe";
    step.action.control = "address";
    step.action.value = "facebook.com";
    step.check.type = ActionType::WebSearch;
    step.check.value = "facebook.com";
    step.expected = "facebook.com";
    std::string error;
    Check(!GoalRunner::ValidateStep(step, error),
        "A web search was accepted as proof of typing in a browser.");
    step.check.type = ActionType::InspectWindow;
    step.check.application = "notepad.exe";
    Check(!GoalRunner::ValidateStep(step, error),
        "An unrelated application's window was accepted as desktop evidence.");
    step.check.application = "msedge.exe";
    Check(GoalRunner::ValidateStep(step, error),
        "A browser edit could not be verified by inspecting its own window.");
}

void TestTheObservationStaysBounded()
{
    auto crowded = MakeObservation();
    for (int index = 0; index < 60; ++index)
    {
        crowded.controls.push_back(MakeControl("item" + std::to_string(index), 10, index));
    }
    crowded.omittedControls = 400;

    const std::string described = crowded.Describe(10);
    Check(described.find("notepad.exe") != std::string::npos,
        "The description did not name the foreground application.");
    // Ten listed, and the rest accounted for: 52 unlisted plus 400 never collected.
    Check(described.find("(452 more not listed)") != std::string::npos,
        "The description did not say how much it left out:\n" + described);

    // A window that puts a paragraph, or a newline, in an accessible name must not be
    // able to break the line it is drawn on or spend the whole budget.
    auto hostile = MakeObservation();
    hostile.controls[0].name = std::string(400, 'x') + "\nSecond line";
    const std::string bounded = hostile.Describe(5);
    Check(bounded.find("Second line") == std::string::npos ||
        bounded.find('\n') != std::string::npos,
        "A multi-line control name was not flattened.");
    Check(bounded.size() < 1200,
        "One long control name was allowed to dominate the description.");

    revia::actions::windows::DesktopObservation blind;
    blind.failure = "Windows UI Automation is unavailable.";
    Check(blind.Describe().find("could not be observed") != std::string::npos,
        "A failed observation did not describe itself as one.");
}

} // namespace

// A goal whose work actually reaches the person, rather than being auto-approved.
// Supervised with a read-only ceiling is what an ordinary desktop session looks like:
// every write asks.
Goal SupervisedGoal(const LoopFixture& fixture, const std::string& title)
{
    Goal goal = fixture.NewGoal(title);
    goal.scope.mode = revia::actions::ExecutionMode::Supervised;
    goal.scope.autoApproveRiskThrough = revia::actions::RiskLevel::ReadOnly;
    return goal;
}

// "Don't ask again" is consent with edges, and the edges are the whole feature.
void TestAStandingYesIsBoundedByTheRunAndByRisk()
{
    LoopFixture fixture;
    int prompts = 0;
    bool grantStanding = true;
    fixture.runner.SetConfirmationHandler([&](
        const revia::actions::ActionRequest&,
        const revia::actions::PolicyDecision&)
    {
        ++prompts;
        // Said once, on the first thing she is asked about.
        return prompts == 1 && grantStanding
            ? revia::actions::ConfirmationChoice::AllowForThisTask
            : revia::actions::ConfirmationChoice::Allow;
    });
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        return iteration >= 3
            ? Done("three directories exist")
            : Take(fixture.MakeDirectory("folder" + std::to_string(iteration)));
    });

    const Goal finished = fixture.runner.Operate(SupervisedGoal(fixture, "three folders"));
    Check(finished.status == GoalStatus::Succeeded,
        "The run did not finish after a standing yes: " + ToString(finished.stopReason));
    Check(finished.steps.size() == 3,
        "The run did not take every step after the standing yes.");
    Check(prompts == 1,
        "A standing yes did not stop the asking; there were " +
            std::to_string(prompts) + " prompts.");

    // It does not survive the run. Consent was given for a task, and that task is over,
    // so a second goal asks about every step again. Answering "once" this time makes the
    // count unambiguous: two steps must produce two questions.
    prompts = 0;
    grantStanding = false;
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        return iteration >= 2
            ? Done("done")
            : Take(fixture.MakeDirectory("later" + std::to_string(iteration)));
    });
    static_cast<void>(fixture.runner.Operate(SupervisedGoal(fixture, "a separate task")));
    Check(prompts == 2,
        "A standing yes leaked past the task it was given for; the next goal asked " +
            std::to_string(prompts) + " times instead of 2.");
}

// The other edge: it covers what was shown and nothing more dangerous.
void TestAStandingYesDoesNotCoverEscalation()
{
    LoopFixture fixture;
    std::vector<revia::actions::RiskLevel> asked;
    fixture.runner.SetConfirmationHandler([&](
        const revia::actions::ActionRequest&,
        const revia::actions::PolicyDecision& decision)
    {
        asked.push_back(decision.risk);
        return revia::actions::ConfirmationChoice::AllowForThisTask;
    });
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        // Two folders, so the deletion can be verified by what SURVIVES it. A check is a
        // read-only action that must find its expected text, which cannot express "the
        // folder is gone" -- so it confirms the other one is still there instead.
        if (iteration == 0) return Take(fixture.MakeDirectory("keep"));
        if (iteration == 1) return Take(fixture.MakeDirectory("scratch"));
        if (iteration == 2)
        {
            // Recycling is classified ReversibleWrite, exactly like the folder creations
            // that were approved -- which is the point. Risk level alone cannot tell these
            // apart, and a yes for one must not be a yes for the other.
            GoalStep step;
            step.description = "Recycle the scratch folder";
            step.action.type = revia::actions::ActionType::MoveToRecycleBin;
            step.action.source = fixture.approved / "scratch";
            step.check.type = revia::actions::ActionType::ListDirectory;
            step.check.source = fixture.approved;
            step.expected = "keep";
            return Take(step);
        }
        return Done("finished");
    });

    const Goal finished = fixture.runner.Operate(SupervisedGoal(fixture, "tidy up"));
    Check(asked.size() == 2,
        "A yes given for ordinary work silently covered a deletion; the person was asked " +
            std::to_string(asked.size()) + " time(s) instead of 2. The run took " +
            std::to_string(finished.steps.size()) + " step(s) and stopped because " +
            ToString(finished.stopReason) + ".");
    Check(asked.back() == revia::actions::RiskLevel::ReversibleWrite,
        "The deletion did not arrive classified the way the risk table classifies it, "
        "which is the whole reason it needs its own rule.");
}

// Answering the up-front goal question with "for the whole task" must actually stop the
// per-step questions, or the offer is a lie and the user keeps clicking.
void TestTheGoalApprovalCanAnswerForTheWholeRun()
{
    LoopFixture fixture;
    int prompts = 0;
    fixture.runner.SetConfirmationHandler([&](
        const revia::actions::ActionRequest&,
        const revia::actions::PolicyDecision&)
    {
        ++prompts;
        return revia::actions::ConfirmationChoice::Allow;
    });
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        return iteration >= 3
            ? Done("done")
            : Take(fixture.MakeDirectory("seeded" + std::to_string(iteration)));
    });

    // What ReviaSession does when the person answers the goal prompt that way.
    fixture.runner.SeedStandingApproval(revia::actions::RiskLevel::ReversibleWrite);
    const Goal finished = fixture.runner.Operate(SupervisedGoal(fixture, "seeded task"));
    Check(finished.status == GoalStatus::Succeeded,
        "The seeded run did not finish: " + ToString(finished.stopReason));
    Check(prompts == 0,
        "Answering for the whole task still produced " + std::to_string(prompts) +
            " per-step prompt(s).");

    // Used once, then gone. A seed that is not renewed does not quietly cover the next
    // goal as well.
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        return iteration >= 1 ? Done("done") : Take(fixture.MakeDirectory("after"));
    });
    static_cast<void>(fixture.runner.Operate(SupervisedGoal(fixture, "the next task")));
    Check(prompts == 1,
        "A seeded approval survived into the following goal.");
}

void RunOperatorLoopTests()
{
    TestDesktopWorkNeedsEvidenceFromItsOwnWindow();
    TestOldGoalDatabaseRetainsItsRecords();
    TestStopDuringPlanningWinsOverLateCompletion();
    TestTheRunIsDiscoveredRatherThanPlanned();
    TestStuckIsNotTheSameAsFinished();
    TestARunWithNoProviderRefuses();
    TestRepeatingTheSameActionStopsTheRun();
    TestDifferentTargetsCountAsProgress();
    TestAnUnverifiableStepIsRefusedMidRun();
    TestVerificationFailureStopsTheRun();
    TestTheLoopCannotWidenItsScope();
    TestBudgetsAndCancellationStillApply();
    TestPlannedRunsAreUnchanged();
    TestAStandingYesIsBoundedByTheRunAndByRisk();
    TestAStandingYesDoesNotCoverEscalation();
    TestTheGoalApprovalCanAnswerForTheWholeRun();
    TestTheObservationDigestNoticesChange();
    TestVerificationEvidenceSurvivesTheHistoryLimit();
    TestExplicitPlannerDecisions();
    TestTheObservationStaysBounded();
    std::cout << "Operator loop tests passed: the run is discovered, verified, and bounded.\n";
}
