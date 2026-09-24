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
    // A goal already in the database keeps the verification contract it was written
    // under. Migrating it to the current one would change the rule its remaining steps
    // are judged by, part-way through a run that was started under the old one.
    Check(store.Load("old")->verificationSchema == LegacyCombinedVerification,
        "An existing goal was silently re-judged under a verification contract it was "
        "not written under.");
}

// The contract is written down, not inferred from the build that happens to read it.
void TestTheVerificationContractRoundTrips()
{
    LoopFixture fixture;
    Goal goal = fixture.NewGoal("Round-trip the contract");
    goal.id = NewGoalId();
    Check(goal.verificationSchema == CurrentVerificationSchema,
        "A new goal did not start under the current verification contract.");
    Check(fixture.store.Save(goal), "The goal did not save.");
    const auto reloaded = fixture.store.Load(goal.id);
    Check(reloaded && reloaded->verificationSchema == CurrentVerificationSchema,
        "The verification contract did not survive a round trip.");

    // And a recorded attempt keeps the descriptive reading alongside the verdict, so a
    // typed pass whose description disagreed is still visible afterwards.
    GoalStep step;
    step.id = NewStepId();
    step.description = "Recycle scratch";
    step.action.type = revia::actions::ActionType::MoveToRecycleBin;
    step.action.source = fixture.approved / "scratch";
    step.check.type = revia::actions::ActionType::ListDirectory;
    step.check.source = fixture.approved;
    step.expected = "scratch is gone";
    StepAttempt attempt;
    attempt.attempt = 1;
    attempt.executed = true;
    attempt.verified = true;
    attempt.outcome = VerificationOutcome::Verified;
    attempt.checkedBy = PostconditionKind::DirectoryLacksEntry;
    attempt.expectedTextSeen = VerificationOutcome::Unknown;
    step.attempts.push_back(attempt);
    goal.steps.push_back(step);
    Check(fixture.store.Save(goal), "The goal with an attempt did not save.");

    const auto back = fixture.store.Load(goal.id);
    Check(back && back->steps.size() == 1 && back->steps[0].attempts.size() == 1,
        "The attempt did not round-trip.");
    const StepAttempt& stored = back->steps[0].attempts[0];
    Check(stored.outcome == VerificationOutcome::Verified &&
            stored.checkedBy == PostconditionKind::DirectoryLacksEntry &&
            stored.expectedTextSeen == VerificationOutcome::Unknown,
        "The descriptive reading was not kept beside the verdict it no longer gates.");
}

// A goal saved mid-run has to come back as the goal that was saved.
//
// Two ways it did not. A PressKeys step reloaded with no chord is not the same step --
// it is an emptier one that would report success for having pressed nothing. And a
// desktop goal reloaded with every hand switched off could never be resumed at all,
// because the scope defaults are all false.
//
// The third case is the one that must NOT round-trip: visual evidence. A region bound
// to a window that was on screen yesterday is a description of a machine that no
// longer exists, and restoring it would be restoring an authorization. It has to come
// back absent so a resumed target is resolved against the screen as it is now.
void TestASavedDesktopGoalKeepsItsPayloadAndScope()
{
    using revia::actions::ActionType;
    using Button = revia::actions::ActionRequest::DesktopInput::PointerButton;
    LoopFixture fixture;

    Goal goal = fixture.NewGoal("Drive a window");
    goal.id = NewGoalId();
    goal.budget.maxIdenticalSteps = 1;
    goal.scope.desktopControl.pointer = true;
    goal.scope.desktopControl.keyboard = true;
    goal.scope.desktopControl.applicationLaunch = true;
    goal.scope.desktopControl.scope =
        revia::actions::CapabilitySettings::DesktopControl::InputScope::WholeDesktop;
    goal.scope.desktopControl.maxTypedCharacters = 64;

    GoalStep chord;
    chord.id = NewStepId();
    chord.description = "Press the save chord";
    chord.action.type = ActionType::PressKeys;
    chord.action.application = "notepad.exe";
    chord.action.input.keys = "ctrl+shift+s";
    chord.check.type = ActionType::InspectWindow;
    chord.check.application = "notepad.exe";
    chord.expected = "Save As";
    goal.steps.push_back(chord);

    GoalStep click;
    click.id = NewStepId();
    click.ordinal = 1;
    click.description = "Click the confirm button";
    click.action.type = ActionType::ClickPointer;
    click.action.application = "notepad.exe";
    click.action.input.x = -1720;
    click.action.input.y = 430;
    click.action.input.hasPoint = true;
    click.action.input.button = Button::Right;
    click.action.input.clickCount = 2;
    // Visual evidence, deliberately not expected back.
    click.action.resolution.kind = revia::actions::TargetResolutionKind::VisualRegion;
    click.action.resolution.observationGeneration = 91;
    click.action.resolution.observationId = "observation-91";
    click.check.type = ActionType::InspectWindow;
    click.check.application = "notepad.exe";
    click.expected = "Saved";
    goal.steps.push_back(click);

    Check(fixture.store.Save(goal), "The desktop goal could not be saved.");
    const auto reloaded = fixture.store.Load(goal.id);
    Check(reloaded.has_value() && reloaded->steps.size() == 2,
        "The saved desktop goal did not reload.");

    Check(reloaded->steps[0].action.input.keys == "ctrl+shift+s",
        "A reloaded chord lost its keys, so the step would press nothing.");
    const auto& point = reloaded->steps[1].action.input;
    Check(point.hasPoint && point.x == -1720 && point.y == 430 &&
        point.button == Button::Right && point.clickCount == 2,
        "A reloaded pointer action lost its point, button, or click count.");
    Check(reloaded->steps[1].action.resolution.kind ==
        revia::actions::TargetResolutionKind::None &&
        reloaded->steps[1].action.resolution.observationGeneration == 0,
        "Stale visual evidence was restored as authorization for a later run.");

    Check(reloaded->budget.maxIdenticalSteps == 1,
        "The anti-loop bound reverted to its default across a save.");
    const auto& control = reloaded->scope.desktopControl;
    Check(control.pointer && control.keyboard && control.applicationLaunch &&
        control.scope ==
            revia::actions::CapabilitySettings::DesktopControl::InputScope::WholeDesktop &&
        control.maxTypedCharacters == 64,
        "A reloaded desktop goal lost the scope it was created with.");
    Check(!control.rawCoordinates && !control.visualTargeting && !control.autonomous &&
        !control.allowCommandSurfaces,
        "A reloaded desktop goal gained an authority it was never granted.");
}

// The same store, reading rows written before either field existed. They have to keep
// loading, and they have to keep loading narrow: an absent scope is no hands, not
// every hand.
void TestLegacyGoalRowsStayNarrow()
{
    LoopFixture fixture;
    Goal legacy = fixture.NewGoal("Written by an older build");
    legacy.id = NewGoalId();
    GoalStep step;
    step.id = NewStepId();
    step.description = "Press a chord";
    step.action.type = revia::actions::ActionType::PressKeys;
    step.action.application = "notepad.exe";
    step.action.input.keys = "ctrl+s";
    step.check.type = revia::actions::ActionType::InspectWindow;
    step.check.application = "notepad.exe";
    step.expected = "Save";
    legacy.steps.push_back(step);
    legacy.scope.desktopControl.keyboard = true;
    Check(fixture.store.Save(legacy), "The legacy fixture could not be saved.");

    // Rewritten as an older build would have stored it: no input on the action, no
    // desktop_control on the scope, no max_identical_steps on the budget. Written
    // literally rather than with json_remove, so the test does not depend on which
    // SQLite extensions this build happens to ship.
    const auto database = (fixture.directory.root / "goals.db").string();
    sqlite3* handle = nullptr;
    Check(sqlite3_open(database.c_str(), &handle) == SQLITE_OK,
        "The legacy fixture store could not be reopened.");
    const std::string strip =
        "UPDATE goals SET budget = '{\"max_actions\":9}', scope = "
        "'{\"mode\":\"approved_scope\"}' WHERE id = '" + legacy.id + "';"
        "UPDATE goal_steps SET action = '{\"type\":\"press_keys\","
        "\"application\":\"notepad.exe\",\"requested_by\":\"goal\"}' WHERE goal_id = '" +
        legacy.id + "';";
    const int result = sqlite3_exec(handle, strip.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(handle);
    Check(result == SQLITE_OK, "The legacy row could not be reduced.");

    const auto reloaded = fixture.store.Load(legacy.id);
    Check(reloaded.has_value(), "A row without the new fields no longer loads.");
    Check(reloaded->budget.maxActions == 9 && reloaded->budget.maxIdenticalSteps ==
        GoalBudget{}.maxIdenticalSteps,
        "An older budget row did not keep its values or its default bound.");
    Check(!reloaded->scope.desktopControl.pointer &&
        !reloaded->scope.desktopControl.keyboard &&
        !reloaded->scope.desktopControl.applicationLaunch,
        "An older row with no recorded scope came back holding the hands.");
    Check(reloaded->steps.size() == 1 && reloaded->steps[0].action.input.keys.empty() &&
        !reloaded->steps[0].action.input.hasPoint,
        "An older step without a payload came back carrying one.");
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

// The action worked, the proof did not arrive, and those are two different facts.
//
// Retrying treats "unverified" as "did not happen". For an action that commits
// something that reading is a coin flip with a duplicate on one side: the copy is made
// twice, the text typed twice, the button pressed twice. The run stops instead, with a
// reason that says the outcome is unknown rather than failed.
void TestAnUncertainEffectIsNotRepeated()
{
    using revia::actions::ActionType;
    LoopFixture fixture;
    const auto source = fixture.approved / "note.txt";
    {
        std::ofstream file(source);
        file << "contents";
        Check(file.good(), "The uncertain-effect fixture could not be written.");
    }

    int attempts = 0;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        ++attempts;
        GoalStep step;
        step.description = "Copy the note";
        step.action.type = ActionType::CopyFile;
        step.action.source = source;
        step.action.destination = fixture.approved / "copy.txt";
        step.check.type = ActionType::ListDirectory;
        step.check.source = fixture.approved;
        // The check runs, succeeds, and does not contain this. Exactly the shape of a
        // receipt going missing.
        step.expected = "a-name-that-is-never-there";
        return Take(step);
    });

    Goal goal = fixture.NewGoal("Copy without proof");
    goal.budget.maxRetriesPerStep = 3;
    const Goal finished = fixture.runner.Operate(goal);

    Check(finished.stopReason == StopReason::UnverifiedEffect,
        "An unknown outcome was recorded as an ordinary verification failure.");
    Check(IsTerminal(finished.status), "The run continued past an unknown outcome.");
    Check(finished.steps.size() == 1 && finished.steps.front().attempts.size() == 1,
        "An action that had already taken effect was attempted again.");
    Check(finished.spend.retries == 0, "A retry was spent on an effect that may have landed.");
    Check(attempts == 1, "The loop asked for another step after an unknown outcome.");
    // The effect really did happen, and the record says so rather than pretending the
    // step was a no-op. That is the whole reason not to repeat it.
    Check(std::filesystem::exists(fixture.approved / "copy.txt") &&
        finished.steps.front().attempts.front().executed,
        "The run hid that the action had already taken effect.");
}

// A failure is not an unknown outcome. Nothing was committed, so the retry budget is
// spent exactly as it was before.
void TestAFailedActionStillRetries()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        GoalStep step = fixture.MakeDirectory("real");
        step.expected = "a-folder-that-was-never-created";
        return Take(step);
    });

    Goal goal = fixture.NewGoal("Claim more than happened");
    goal.budget.maxRetriesPerStep = 1;
    const Goal finished = fixture.runner.Operate(goal);

    Check(finished.stopReason == StopReason::VerificationFailed,
        "A repeatable action stopped as though its outcome were unknown.");
    Check(finished.steps.front().attempts.size() == 2,
        "The retry budget was not spent on an action that commits nothing.");
}

// The process died between acting and observing. From here that is indistinguishable
// from having died before acting, so a step that commits something is not replayed into
// a machine nobody has looked at since.
void TestARestartDoesNotReplayAnUncertainEffect()
{
    using revia::actions::ActionType;
    LoopFixture fixture;

    Goal interrupted = fixture.NewGoal("Interrupted mid-type");
    interrupted.id = NewGoalId();
    interrupted.status = GoalStatus::Running;
    GoalStep typing;
    typing.id = NewStepId();
    typing.description = "Type the address";
    typing.action.type = ActionType::TypeText;
    typing.action.application = "msedge.exe";
    typing.action.value = "facebook.com";
    typing.check.type = ActionType::InspectWindow;
    typing.check.application = "msedge.exe";
    typing.expected = "facebook.com";
    typing.status = StepStatus::Acting;
    interrupted.steps.push_back(typing);
    Check(fixture.store.Save(interrupted), "The interrupted goal could not be saved.");

    const Goal resumed = fixture.runner.Resume(interrupted.id);
    Check(resumed.stopReason == StopReason::UnverifiedEffect,
        "A restart replayed an action whose outcome was never established.");
    Check(resumed.steps.size() == 1 && resumed.steps.front().attempts.empty(),
        "The interrupted action was attempted again after the restart.");
    // Terminal, so asking again cannot find the step out of flight and replay it.
    Check(IsTerminal(resumed.status), "An unresolved effect was left resumable.");
    const Goal askedAgain = fixture.runner.Resume(interrupted.id);
    Check(askedAgain.steps.size() == 1 && askedAgain.steps.front().attempts.empty(),
        "Resuming a second time replayed the action the first resume refused.");

    // The contrast: a step that only re-reaches a state resumes exactly as it did
    // before, because repeating it costs nothing.
    Goal repeatable = fixture.NewGoal("Interrupted mid-create");
    repeatable.id = NewGoalId();
    repeatable.status = GoalStatus::Running;
    GoalStep make = fixture.MakeDirectory("resumed");
    make.id = NewStepId();
    make.status = StepStatus::Acting;
    repeatable.steps.push_back(make);
    Check(fixture.store.Save(repeatable), "The repeatable goal could not be saved.");

    const Goal continued = fixture.runner.Resume(repeatable.id);
    Check(continued.status == GoalStatus::Succeeded &&
        std::filesystem::is_directory(fixture.approved / "resumed"),
        "A restart refused to continue work that could safely be repeated.");
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

// The parser's contract is that every bad input comes back as a bounded failure. A
// field of the wrong type used to leave through an exception instead, because these
// reads sit outside the handler that guards the initial parse.
void TestMalformedNextStepFieldsFailBounded()
{
    using revia::planning::GoalPlanner;
    for (const char* malformed : {
            R"({"finished":"yes"})",
            R"({"finished":1})",
            R"({"finished":true,"reason":42})",
            R"({"reason":["not","a","string"]})",
            R"({"description":7,"action":{"action":"inspect_window","application":"a.exe"},
                "check":{"action":"inspect_window","application":"a.exe"},"expected":"a"})",
            R"({"expected":42,"action":{"action":"inspect_window","application":"a.exe"},
                "check":{"action":"inspect_window","application":"a.exe"}})"})
    {
        const auto parsed = GoalPlanner::ParseNextStep(malformed);
        Check(!parsed.succeeded && !parsed.error.empty(),
            std::string("A malformed field was accepted or threw instead of failing: ") +
                malformed);
    }

    // The well-formed shapes around them still parse, so the check is a type check and
    // not a new requirement that every field be present.
    const auto minimal = GoalPlanner::ParseNextStep(R"({"finished":true})");
    Check(minimal.succeeded && minimal.finished,
        "A completion with no reason stopped parsing.");
    const auto nothingToDo = GoalPlanner::ParseNextStep(R"({"reason":"Nothing to do."})");
    Check(nothingToDo.succeeded && !nothingToDo.finished,
        "A no-action answer with a reason stopped parsing.");
}

// Verification, asked as a question the evidence can answer.
//
// The original rule searched the check's output for a substring of `expected`. That is
// satisfied by text that was already there, by a sentence saying the opposite, and by
// "MyNotes.txt" when the step created "Notes". These tests are about the difference
// between searching and checking, and about the third answer neither of them had: "I
// could not tell", which is not the same as "no".

revia::actions::ActionResult Listing(const std::vector<std::string>& names)
{
    revia::actions::ActionResult result;
    result.attempted = true;
    result.succeeded = true;
    for (const std::string& name : names) result.entries.push_back("[FILE]  " + name);
    return result;
}

revia::actions::ActionResult Window(
    const std::string& application,
    const std::string& foreground,
    const std::vector<std::string>& controls)
{
    revia::actions::ActionResult result;
    result.attempted = true;
    result.succeeded = true;
    result.content = "Application: " + application + "\nWindow: Some window";
    if (!foreground.empty()) result.content += "\nForeground: " + foreground;
    result.entries = controls;
    return result;
}

// The false positive the typed check exists to remove, through the real runner.
//
// The step creates "Notes". A sibling called "MyNotes" is already there. The old rule
// searched the listing for "Notes" and found it inside "MyNotes", so a step could have
// been verified by a directory it did not create -- and here the creation is made to
// fail outright, so there is nothing of its own to find.
// A budget that is never charged is a number in a struct, not a limit.
//
// maxTokens was compared against a spend that nothing ever incremented, so it could not
// stop anything however long a run went on. These check that planning is now paid for,
// and that the bound still holds when the backend refuses to say what anything cost.
void TestPlanningIsChargedToTheBudget()
{
    LoopFixture fixture;
    int asked = 0;
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        ++asked;
        NextStep next = iteration == 0
            ? Take(fixture.MakeDirectory("first"))
            : Done("that is all");
        next.tokens = 400;
        next.costReported = true;
        return next;
    });

    Goal goal = fixture.NewGoal("Spend a little");
    const Goal finished = fixture.runner.Operate(goal);
    Check(finished.status == GoalStatus::Succeeded, "The short run did not finish.");
    Check(finished.spend.plannerRequests == static_cast<std::uint32_t>(asked) && asked == 2,
        "Planning requests were not counted.");
    Check(finished.spend.tokens == 800 && finished.spend.unreportedRequests == 0,
        "Reported planning cost was not charged to the run.");
}

void TestTheTokenBudgetCanActuallyStopARun()
{
    LoopFixture fixture;
    int asked = 0;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        ++asked;
        // Always something new to do, so nothing but a budget can end this.
        NextStep next = Take(fixture.MakeDirectory("step" + std::to_string(asked)));
        next.tokens = 500;
        next.costReported = true;
        return next;
    });

    Goal goal = fixture.NewGoal("Spend until stopped");
    goal.budget.maxTokens = 1200;
    goal.budget.maxActions = 0;      // unbounded, so the token ceiling is what bites
    goal.budget.maxDurationMs = 0;
    const Goal finished = fixture.runner.Operate(goal);

    Check(finished.stopReason == StopReason::BudgetTokens &&
        finished.status == GoalStatus::Exhausted,
        "The token budget did not stop a run that kept spending.");
    Check(finished.spend.tokens > goal.budget.maxTokens,
        "The run stopped before it had actually exceeded its budget.");
    Check(asked < 10, "The run spent far past its ceiling before noticing.");
}

// The case the request ceiling exists for. Nothing reports usage, so the token count
// stays at zero forever and could never reach any limit.
void TestAnUnreportedBackendIsStillBounded()
{
    LoopFixture fixture;
    int asked = 0;
    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        ++asked;
        // costReported stays false: the backend said nothing about what this cost.
        return Take(fixture.MakeDirectory("step" + std::to_string(asked)));
    });

    Goal goal = fixture.NewGoal("Spend invisibly");
    goal.budget.maxTokens = 1200;
    goal.budget.maxPlannerRequests = 4;
    goal.budget.maxActions = 0;
    goal.budget.maxDurationMs = 0;
    const Goal finished = fixture.runner.Operate(goal);

    Check(finished.stopReason == StopReason::BudgetTokens &&
        finished.status == GoalStatus::Exhausted,
        "A backend that reports no usage ran without any ceiling at all.");
    Check(finished.spend.tokens == 0 &&
        finished.spend.unreportedRequests == finished.spend.plannerRequests,
        "An unreported cost was invented rather than recorded as unknown.");
    Check(asked <= 6, "The request ceiling did not bound the run tightly.");

    // And the accounting survives being stored, so a past run's cost can be read back
    // as the partial statement it is.
    const auto reloaded = fixture.store.Load(finished.id);
    Check(reloaded && reloaded->spend.plannerRequests == finished.spend.plannerRequests &&
        reloaded->spend.unreportedRequests == finished.spend.unreportedRequests &&
        reloaded->budget.maxPlannerRequests == 4,
        "The planning accounting did not survive being stored.");
}

void TestUnrelatedEvidenceNoLongerVerifiesAStep()
{
    using revia::actions::ActionType;
    LoopFixture fixture;
    // The step will create sub/Notes and then look in the wrong directory -- the one
    // above it, which happens to hold a folder whose name contains "Notes". The action
    // genuinely succeeds, so this is not a failure being caught; it is a success being
    // claimed from evidence that has nothing to do with it.
    std::filesystem::create_directory(fixture.approved / "MyNotes");
    std::filesystem::create_directory(fixture.approved / "sub");

    fixture.runner.SetStepProvider([&](const Goal&, std::uint32_t)
    {
        GoalStep step;
        step.description = "Create Notes";
        step.action.type = ActionType::CreateDirectory;
        step.action.source = fixture.approved / "sub" / "Notes";
        // Looking one level too high. The old rule searched this listing for "Notes"
        // and found it inside "MyNotes".
        step.check.type = ActionType::ListDirectory;
        step.check.source = fixture.approved;
        step.expected = "Notes";
        return Take(step);
    });

    Goal goal = fixture.NewGoal("Create a folder and check the wrong place");
    goal.budget.maxRetriesPerStep = 0;
    const Goal finished = fixture.runner.Operate(goal);

    Check(std::filesystem::is_directory(fixture.approved / "sub" / "Notes"),
        "The fixture did not actually perform the action it is verifying.");
    Check(!finished.steps.empty() && !finished.steps.front().attempts.empty(),
        "The run recorded no attempt.");
    const StepAttempt& attempt = finished.steps.front().attempts.front();
    Check(attempt.executed, "The action did not run, so nothing was verified either way.");
    // The record says which question was asked, so a past run's strength is readable
    // afterwards rather than guessed at.
    Check(attempt.checkedBy == PostconditionKind::DirectoryHasEntry,
        "The step was checked by the weak rule when a typed one was available.");
    // Failed rather than Unknown: the listing was read, and the name is positively not
    // in it. That is what makes it safe to say no.
    Check(attempt.outcome == VerificationOutcome::Failed && !attempt.verified,
        "A step was verified by a folder it did not create.");
    Check(finished.stopReason == StopReason::VerificationFailed,
        "The run did not stop for the reason the check actually gave.");
}

// And the ordinary case still passes, by both questions at once.
void TestAStepStillVerifiesWhenItReallyWorked()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        if (iteration == 0) return Take(fixture.MakeDirectory("Notes"));
        return Done("the folder exists");
    });
    const Goal finished = fixture.runner.Operate(fixture.NewGoal("Create Notes"));
    Check(finished.status == GoalStatus::Succeeded,
        "A step that genuinely worked stopped verifying.");
    const StepAttempt& attempt = finished.steps.front().attempts.front();
    Check(attempt.verified && attempt.outcome == VerificationOutcome::Verified &&
        attempt.checkedBy == PostconditionKind::DirectoryHasEntry,
        "A verified step lost its recorded outcome or the question that established it.");

    // And the record survives a round trip, so what a past run could actually tell is
    // readable later rather than reconstructed from a boolean.
    const auto reloaded = fixture.store.Load(finished.id);
    Check(reloaded && reloaded->steps.front().attempts.front().outcome ==
        VerificationOutcome::Verified &&
        reloaded->steps.front().attempts.front().checkedBy ==
            PostconditionKind::DirectoryHasEntry,
        "The verification outcome did not survive being stored.");
}

void TestAListingIsCheckedByNameAndNotBySubstring()
{
    Postcondition created;
    created.kind = PostconditionKind::DirectoryHasEntry;
    created.value = "Notes";

    Check(EvaluatePostcondition(created, Listing({"Notes"})) == VerificationOutcome::Verified,
        "An entry that was actually created did not verify.");
    // The whole point. Every one of these satisfies a substring search for "Notes".
    for (const auto& impostor : {"MyNotes", "Notes-old", "Notes.txt", "NotesBackup"})
    {
        Check(EvaluatePostcondition(created, Listing({impostor})) ==
            VerificationOutcome::Failed,
            std::string("A different entry was accepted as proof: ") + impostor);
    }
    Check(EvaluatePostcondition(created, Listing({"Other", "Notes", "Third"})) ==
        VerificationOutcome::Verified,
        "The entry was not found beside unrelated ones.");

    Postcondition removed;
    removed.kind = PostconditionKind::DirectoryLacksEntry;
    removed.value = "Notes";
    Check(EvaluatePostcondition(removed, Listing({"Other"})) == VerificationOutcome::Verified,
        "A removal was not verified by the name being gone.");
    Check(EvaluatePostcondition(removed, Listing({"Notes"})) == VerificationOutcome::Failed,
        "A removal verified while the entry was still there.");
}

// A check that did not run establishes nothing. This is the difference that decides
// whether an action may be repeated, so it must never read as a positive "no".
void TestAFailedCheckEstablishesNothing()
{
    revia::actions::ActionResult unread;
    unread.attempted = true;
    unread.succeeded = false;
    unread.message = "The directory could not be read.";

    for (const auto kind : {PostconditionKind::DirectoryHasEntry,
            PostconditionKind::DirectoryLacksEntry, PostconditionKind::FileContains,
            PostconditionKind::ForegroundApplicationIs, PostconditionKind::ControlValueIs,
            PostconditionKind::TextObserved})
    {
        Postcondition condition;
        condition.kind = kind;
        condition.subject = "anything";
        condition.value = "anything";
        Check(EvaluatePostcondition(condition, unread) == VerificationOutcome::Unknown,
            "A check that did not run was read as an answer.");
    }
}

// Finding a window is not the same as that window being in front.
void TestForegroundIdentityNeedsBothFacts()
{
    Postcondition inFront;
    inFront.kind = PostconditionKind::ForegroundApplicationIs;
    inFront.value = "msedge.exe";

    Check(EvaluatePostcondition(inFront, Window("msedge.exe", "true", {})) ==
        VerificationOutcome::Verified,
        "The application in front did not verify.");
    Check(EvaluatePostcondition(inFront, Window("notepad.exe", "true", {})) ==
        VerificationOutcome::Failed,
        "A different application was accepted as the one in front.");
    Check(EvaluatePostcondition(inFront, Window("msedge.exe", "false", {})) ==
        VerificationOutcome::Failed,
        "An application that was found but not in front was accepted anyway.");
    // The inspection did not report whether it was in front. That is not a statement
    // that it was not.
    Check(EvaluatePostcondition(inFront, Window("msedge.exe", "", {})) ==
        VerificationOutcome::Unknown,
        "A missing foreground line was treated as a verdict.");
}

// Composing is not sending, and a value that cannot be read is not a value that is
// wrong.
void TestAControlValueIsCheckedExactly()
{
    Postcondition typed;
    typed.kind = PostconditionKind::ControlValueIs;
    typed.subject = "address";
    typed.value = "facebook.com";

    Check(EvaluatePostcondition(typed, Window("msedge.exe", "true",
            {"address [type=50004, enabled=true, value=facebook.com]"})) ==
        VerificationOutcome::Verified,
        "Text that reached the box did not verify.");
    Check(EvaluatePostcondition(typed, Window("msedge.exe", "true",
            {"address [type=50004, enabled=true, value=facebook.com/other]"})) ==
        VerificationOutcome::Failed,
        "A different value was accepted because it contained the expected one.");
    // Matched by automation id rather than by name.
    Check(EvaluatePostcondition(typed, Window("msedge.exe", "true",
            {"Address and search bar [type=50004, enabled=true, id=address, value=facebook.com]"})) ==
        VerificationOutcome::Verified,
        "A control identified by its automation id was not found.");
    // A password field reports no value at all. Unknown, emphatically not Failed.
    Check(EvaluatePostcondition(typed, Window("msedge.exe", "true",
            {"address [type=50004, enabled=true]"})) == VerificationOutcome::Unknown,
        "A control whose value could not be read was reported as the wrong value.");
    // The listing is capped, so a control that is not in it may still be on screen.
    Check(EvaluatePostcondition(typed, Window("msedge.exe", "true",
            {"Something else [type=50000, enabled=true]"})) == VerificationOutcome::Unknown,
        "A control missing from a capped listing was reported as missing from the screen.");
}

// The legacy rule keeps working and keeps being weak, and never answers "no". Callers
// use a positive "no" to decide that repeating an action is safe, and a substring that
// is not found is not a finding.
void TestTheLegacyRuleNeverClaimsAFailure()
{
    Postcondition legacy;
    legacy.kind = PostconditionKind::TextObserved;
    legacy.value = "Notes";
    Check(EvaluatePostcondition(legacy, Listing({"MyNotes"})) == VerificationOutcome::Verified,
        "The legacy rule stopped matching a substring, which existing steps rely on.");
    Check(EvaluatePostcondition(legacy, Listing({"Other"})) == VerificationOutcome::Unknown,
        "The legacy rule claimed a failure it cannot establish.");
    Check(!legacy.IsTyped() && Postcondition{PostconditionKind::DirectoryHasEntry,
        {}, "x"}.IsTyped(), "Typed and legacy conditions were not distinguishable.");
}

// Derived from the step's own action and check, never supplied. A step that sends
// something gets no typed condition rather than a flattering one.
void TestPostconditionsAreDerivedFromTheStep()
{
    using revia::actions::ActionType;
    const auto derive = [](const ActionType action, const ActionType check,
        const std::string& source, const std::string& destination,
        const std::string& application, const std::string& control,
        const std::string& value, const std::string& expected)
    {
        GoalStep step;
        step.action.type = action;
        step.action.source = revia::actions::Utf8ToPath(source);
        step.action.destination = revia::actions::Utf8ToPath(destination);
        step.action.application = application;
        step.action.control = control;
        step.action.value = value;
        step.check.type = check;
        step.expected = expected;
        return DerivePostcondition(step);
    };

    const auto created = derive(ActionType::CreateDirectory, ActionType::ListDirectory,
        "C:/Sandbox/Notes", "", "", "", "", "Notes");
    Check(created.kind == PostconditionKind::DirectoryHasEntry && created.value == "Notes",
        "Creating a directory did not derive a check for that name.");

    const auto moved = derive(ActionType::MoveFile, ActionType::ListDirectory,
        "C:/Sandbox/a.txt", "C:/Sandbox/Archive/b.txt", "", "", "", "b.txt");
    Check(moved.kind == PostconditionKind::DirectoryHasEntry && moved.value == "b.txt",
        "Moving a file did not derive a check for where it landed.");

    const auto recycled = derive(ActionType::MoveToRecycleBin, ActionType::ListDirectory,
        "C:/Sandbox/gone.txt", "", "", "", "", "gone.txt");
    Check(recycled.kind == PostconditionKind::DirectoryLacksEntry &&
        recycled.value == "gone.txt",
        "Recycling a file did not derive a check for its absence.");

    const auto launched = derive(ActionType::LaunchApplication, ActionType::InspectWindow,
        "", "", "msedge.exe", "", "", "Edge");
    Check(launched.kind == PostconditionKind::ForegroundApplicationIs &&
        launched.value == "msedge.exe",
        "Launching an application did not derive a check for it being in front.");

    const auto typed = derive(ActionType::TypeText, ActionType::InspectWindow,
        "", "", "msedge.exe", "address", "facebook.com", "facebook.com");
    Check(typed.kind == PostconditionKind::ControlValueIs && typed.subject == "address" &&
        typed.value == "facebook.com",
        "Typing into a named control did not derive a check on that control.");

    // Pressing Enter to send is an effect with no evidence in a window inspection. It
    // gets the weak check rather than one it would pass for the wrong reason, and the
    // weak check can never answer "no", so a send that cannot be confirmed stops
    // instead of being repeated.
    const auto sent = derive(ActionType::PressKeys, ActionType::InspectWindow,
        "", "", "msedge.exe", "", "", "Sent");
    Check(!sent.IsTyped(), "Sending was given a typed check it cannot actually support.");

    // Nothing recognised: the behaviour every step had before this existed.
    const auto unknown = derive(ActionType::ClickPointer, ActionType::ListDirectory,
        "", "", "", "", "", "whatever");
    Check(!unknown.IsTyped() && unknown.value == "whatever",
        "An underived step lost its original expectation.");
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

// ISSUE-REVIA-0069. A deletion is proved by the name being gone, not by finding it.
//
// The natural way to write this step is to say the thing that should no longer be
// there. Under the combined rule that was self-defeating: the typed condition asked
// whether "scratch" was absent from the listing and got yes, then the descriptive text
// was searched for "scratch" in that same listing and could not find it -- because the
// deletion had worked. The step came back Unknown, the run stopped as an unverified
// effect, and a goal that did exactly what was asked was recorded as a failure.
void TestADeletionIsProvedByItsAbsence()
{
    LoopFixture fixture;
    fixture.runner.SetStepProvider([&](const Goal&, const std::uint32_t iteration)
    {
        if (iteration == 0) return Take(fixture.MakeDirectory("scratch"));
        if (iteration == 1)
        {
            GoalStep step;
            step.description = "Recycle the scratch folder";
            step.action.type = revia::actions::ActionType::MoveToRecycleBin;
            step.action.source = fixture.approved / "scratch";
            step.check.type = revia::actions::ActionType::ListDirectory;
            step.check.source = fixture.approved;
            // What the step is actually for, said plainly. Naming the thing that should
            // be gone is the obvious description and used to be the one that broke it.
            step.expected = "scratch is gone";
            return Take(step);
        }
        return Done("the folder is gone");
    });

    Goal goal = fixture.NewGoal("remove scratch");
    goal.scope.autoApproveRiskThrough = revia::actions::RiskLevel::ReversibleWrite;
    const Goal finished = fixture.runner.Operate(goal);

    Check(!std::filesystem::exists(fixture.approved / "scratch"),
        "The deletion did not actually happen, so this proves nothing about verifying it.");
    Check(finished.status == GoalStatus::Succeeded,
        "A deletion that worked was not accepted: the run stopped because " +
            ToString(finished.stopReason) + ".");
    const StepAttempt& attempt = finished.steps.at(1).attempts.back();
    Check(attempt.outcome == VerificationOutcome::Verified,
        "Absence was not accepted as proof of a deletion; the outcome was " +
            ToString(attempt.outcome) + ".");
    Check(attempt.checkedBy == PostconditionKind::DirectoryLacksEntry,
        "The deletion was judged by the wrong question.");
    // The descriptive text is still evaluated and still recorded. It says what it always
    // said here -- the name is not in the listing -- and that is now a diagnostic rather
    // than a veto.
    Check(attempt.expectedTextSeen == VerificationOutcome::Unknown,
        "The descriptive reading was not preserved for the record.");
}

// The other direction, and the reason the typed condition is not simply trusted.
//
// A step that announces one thing and acts on another would otherwise be graded on what
// it did rather than on what it said, and pass. The typed condition has to be about the
// step before it can be the contract.
void TestATypedPassAboutSomethingElseProvesNothing()
{
    revia::actions::ActionResult listing;
    listing.succeeded = true;
    listing.entries = {"[DIR]   real"};

    Postcondition created;
    created.kind = PostconditionKind::DirectoryHasEntry;
    created.value = "real";

    const VerificationJudgement unrelated = JudgeStep(
        CurrentVerificationSchema, created, "a-folder-that-was-never-created", listing);
    Check(!unrelated.typedIsRelevant,
        "A condition about one folder counted as a claim about another.");
    Check(unrelated.outcome == VerificationOutcome::Unknown,
        "A step graded itself on the thing it did instead of the thing it promised.");

    const VerificationJudgement corroborated =
        JudgeStep(CurrentVerificationSchema, created, "real", listing);
    Check(corroborated.typedIsRelevant &&
            corroborated.outcome == VerificationOutcome::Verified,
        "A step that did what it said it would do was not verified.");
}

// A goal written under the old contract is judged under the old contract, however new
// the code reading it is.
void TestTheLegacyVerificationContractIsPreserved()
{
    revia::actions::ActionResult listing;
    listing.succeeded = true;
    // The deletion worked: the name is not here.
    listing.entries = {"[DIR]   keep"};

    Postcondition removed;
    removed.kind = PostconditionKind::DirectoryLacksEntry;
    removed.value = "scratch";

    const VerificationJudgement legacy =
        JudgeStep(LegacyCombinedVerification, removed, "scratch is gone", listing);
    Check(legacy.outcome == VerificationOutcome::Unknown,
        "A goal written under the combined rule was silently re-judged under the new one.");

    const VerificationJudgement current =
        JudgeStep(CurrentVerificationSchema, removed, "scratch is gone", listing);
    Check(current.outcome == VerificationOutcome::Verified,
        "The current contract did not accept absence as proof of a deletion.");
}

// A shape the derivation does not recognise is judged exactly as it always was.
void TestUnrecognisedShapesKeepTheLegacyRule()
{
    revia::actions::ActionResult result;
    result.succeeded = true;
    result.content = "the window says Ready";

    Postcondition none;
    none.kind = PostconditionKind::TextObserved;
    none.value = "Ready";

    const VerificationJudgement seen =
        JudgeStep(CurrentVerificationSchema, none, "Ready", result);
    Check(seen.outcome == VerificationOutcome::Verified && !seen.typedIsRelevant,
        "The legacy substring rule stopped working for the steps that still depend on it.");

    Postcondition absent = none;
    absent.value = "Finished";
    const VerificationJudgement unseen =
        JudgeStep(CurrentVerificationSchema, absent, "Finished", result);
    Check(unseen.outcome == VerificationOutcome::Unknown,
        "The legacy rule started claiming a failure it cannot establish.");
}

// A typed "no" survives irrelevance, because it is the only thing that makes a retry
// safe. Nothing landed, so doing it again does it once.
void TestATypedFailureIsCarriedEvenWhenUncorroborated()
{
    revia::actions::ActionResult listing;
    listing.succeeded = true;
    listing.entries = {"[DIR]   something-else"};

    Postcondition created;
    created.kind = PostconditionKind::DirectoryHasEntry;
    created.value = "real";

    const VerificationJudgement judged =
        JudgeStep(CurrentVerificationSchema, created, "an unrelated description", listing);
    Check(!judged.typedIsRelevant && judged.outcome == VerificationOutcome::Failed,
        "An action that demonstrably did not happen was downgraded to uncertain, which "
        "is what turns a safe retry into a refusal to continue.");
}

void RunOperatorLoopTests()
{
    TestADeletionIsProvedByItsAbsence();
    TestATypedPassAboutSomethingElseProvesNothing();
    TestTheLegacyVerificationContractIsPreserved();
    TestUnrecognisedShapesKeepTheLegacyRule();
    TestATypedFailureIsCarriedEvenWhenUncorroborated();
    TestDesktopWorkNeedsEvidenceFromItsOwnWindow();
    TestPlanningIsChargedToTheBudget();
    TestTheTokenBudgetCanActuallyStopARun();
    TestAnUnreportedBackendIsStillBounded();
    TestUnrelatedEvidenceNoLongerVerifiesAStep();
    TestAStepStillVerifiesWhenItReallyWorked();
    TestAListingIsCheckedByNameAndNotBySubstring();
    TestAFailedCheckEstablishesNothing();
    TestForegroundIdentityNeedsBothFacts();
    TestAControlValueIsCheckedExactly();
    TestTheLegacyRuleNeverClaimsAFailure();
    TestPostconditionsAreDerivedFromTheStep();
    TestMalformedNextStepFieldsFailBounded();
    TestASavedDesktopGoalKeepsItsPayloadAndScope();
    TestLegacyGoalRowsStayNarrow();
    TestOldGoalDatabaseRetainsItsRecords();
    TestTheVerificationContractRoundTrips();
    TestStopDuringPlanningWinsOverLateCompletion();
    TestTheRunIsDiscoveredRatherThanPlanned();
    TestStuckIsNotTheSameAsFinished();
    TestARunWithNoProviderRefuses();
    TestRepeatingTheSameActionStopsTheRun();
    TestDifferentTargetsCountAsProgress();
    TestAnUnverifiableStepIsRefusedMidRun();
    TestVerificationFailureStopsTheRun();
    TestAnUncertainEffectIsNotRepeated();
    TestAFailedActionStillRetries();
    TestARestartDoesNotReplayAnUncertainEffect();
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
