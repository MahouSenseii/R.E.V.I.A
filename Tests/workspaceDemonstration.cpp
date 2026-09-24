#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Goals/goalRunner.h"
#include "Goals/goalStore.h"
#include "Policy/desktopAuthorization.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace
{

using namespace revia::goals;
using revia::tests::Check;

// A runnable demonstration rather than an assertion suite.
//
// It shows the two halves that have to be true at once: she finishes a multi-step job in
// a disposable workspace without asking about any of it, and she stops at a boundary
// with a reason instead of finding a way through.
//
// Honest scope: this drives GoalRunner::Operate directly and uses filesystem actions.
// It is not a live desktop run, and it is not reached from a session command, because
// the operator loop is not wired into the session in this build.

void Say(const std::string& line)
{
    std::cout << "  " << line << "\n";
}

nlohmann::json Capabilities(const std::filesystem::path& approvedRoot)
{
    return {
        {"mode", "approved_scope"},
        {"approvedRoots", {revia::actions::PathToUtf8(approvedRoot)}},
        {"autoApproveRiskThrough", "reversible_write"},
        {"createMissingApprovedRoots", false}};
}

} // namespace

void RunWorkspaceDemonstration()
{
    revia::tests::ScopedTestDirectory directory;
    const auto workspace = directory.root / "ReviaScratch";
    const auto offLimits = directory.root / "not-approved";
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(offLimits);

    const auto configPath = directory.root / "capabilities.json";
    {
        std::ofstream file(configPath);
        file << Capabilities(workspace).dump();
    }

    revia::actions::ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configPath, directory.root / "audit.jsonl", error),
        "The demonstration could not initialize the action runtime: " + error);

    const GoalStore store((directory.root / "goals.db").string());
    GoalRunner runner(runtime, store);

    std::cout << "\n=== Revia, disposable workspace ===\n";
    std::cout << "Workspace: " << revia::actions::PathToUtf8(workspace) << "\n\n";

    // ---------------------------------------------------------------- part one
    std::cout << "1. A job she can finish on her own.\n";
    int asked = 0;
    runner.SetStepProvider([&](const Goal& goal, const std::uint32_t iteration) -> NextStep
    {
        ++asked;
        NextStep next;
        const auto folder = [&](const std::string& name)
        {
            GoalStep step;
            step.description = "Create " + name;
            step.action.type = revia::actions::ActionType::CreateDirectory;
            step.action.source = workspace / name;
            step.check.type = revia::actions::ActionType::ListDirectory;
            step.check.source = workspace;
            step.expected = name;
            return step;
        };
        // Each decision is made after seeing what the last one actually achieved.
        if (iteration == 0) { next.hasStep = true; next.step = folder("drafts"); }
        else if (iteration == 1) { next.hasStep = true; next.step = folder("archive"); }
        else { next.finished = true; next.reason = "both folders exist"; }
        Say(next.hasStep
            ? "step " + std::to_string(iteration + 1) + ": " + next.step.description
            : "done: " + next.reason + " (after " + std::to_string(goal.steps.size()) +
                " verified steps)");
        return next;
    });

    Goal tidy;
    tidy.title = "Set up the scratch workspace";
    tidy.scope = NarrowScopeForGoal(runtime.Settings());
    const Goal finishedTidy = runner.Operate(tidy);

    Check(finishedTidy.status == GoalStatus::Succeeded,
        "The demonstration's ordinary task did not complete.");
    Check(std::filesystem::is_directory(workspace / "drafts") &&
        std::filesystem::is_directory(workspace / "archive"),
        "The demonstration reported success without doing the work.");
    Say("no approval was requested for any of it");
    Say("each step proved itself with a read-only check before the next was chosen");

    // ---------------------------------------------------------------- part two
    std::cout << "\n2. The same loop, asked to step outside the workspace.\n";
    runner.SetStepProvider([&](const Goal&, std::uint32_t) -> NextStep
    {
        NextStep next;
        next.hasStep = true;
        next.step.description = "Create a folder outside the workspace";
        next.step.action.type = revia::actions::ActionType::CreateDirectory;
        next.step.action.source = offLimits / "escaped";
        next.step.check.type = revia::actions::ActionType::ListDirectory;
        next.step.check.source = offLimits;
        next.step.expected = "escaped";
        Say("step 1: " + next.step.description);
        return next;
    });

    Goal overreach;
    overreach.title = "Reach outside the workspace";
    overreach.scope = NarrowScopeForGoal(runtime.Settings());
    const Goal stopped = runner.Operate(overreach);

    Check(stopped.status == GoalStatus::Blocked &&
        stopped.stopReason == StopReason::PolicyBlocked,
        "The demonstration's boundary was not enforced.");
    Check(!std::filesystem::exists(offLimits / "escaped"),
        "A blocked step still reached the filesystem.");
    Say("blocked: " + ToString(stopped.stopReason));
    Say("nothing was created outside the workspace");
    Say("she stopped rather than looking for another route to the same effect");

    // ---------------------------------------------------------------- part three
    std::cout << "\n3. The consequence gate, on the same question asked two ways.\n";
    revia::actions::CapabilitySettings::DesktopControl desktop;
    desktop.pointer = true;
    desktop.keyboard = true;

    const auto ask = [&](const revia::policy::DesktopOperation operation,
        const std::string& label, const char* routeName)
    {
        revia::policy::AuthorizationRequest request;
        request.operation = operation;
        request.evidence.resolved = true;
        request.evidence.controlName = label;
        request.autonomousOrigin = true;
        const auto decision = revia::policy::AuthorizeDesktopEffect(request, desktop);
        Say(std::string(routeName) + " on \"" + label + "\" -> " +
            ToString(decision.verdict));
        return decision;
    };

    const auto clicked = ask(revia::policy::DesktopOperation::PointerActivate, "Send", "click");
    const auto invoked = ask(revia::policy::DesktopOperation::Invoke, "Send", "UIA invoke");
    Check(clicked.verdict == invoked.verdict,
        "The same effect cost different authority by route.");
    Say("same verdict either way: the route is not a property of the consequence");

    const auto ordinary = ask(
        revia::policy::DesktopOperation::PointerActivate, "Zoom in", "click");
    Check(ordinary.verdict == revia::policy::AuthorizationVerdict::Allow,
        "An ordinary control required approval.");
    Say("ordinary controls stay ordinary; the gate is not a permission prompt on everything");

    std::cout << "\n=== end ===\n\n";
}
