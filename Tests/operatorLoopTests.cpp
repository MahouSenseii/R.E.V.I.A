#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Goals/goalRunner.h"
#include "Goals/goalStore.h"
#include "Windows/desktopObserver.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
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
    // The work it did manage still stands and is still recorded.
    Check(std::filesystem::is_directory(fixture.approved / "only") &&
        finished.steps.size() == 1 && finished.steps.front().attempts.front().verified,
        "Stopping undecided discarded the step that did succeed.");
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

void RunOperatorLoopTests()
{
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
    TestTheObservationDigestNoticesChange();
    TestTheObservationStaysBounded();
    std::cout << "Operator loop tests passed: the run is discovered, verified, and bounded.\n";
}
