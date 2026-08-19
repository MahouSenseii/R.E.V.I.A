#include "Goals/goalSandbox.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace revia::goals
{

namespace
{

SandboxRehearsal Unsupported(std::string reason)
{
    SandboxRehearsal result;
    result.supported = false;
    result.reason = std::move(reason);
    return result;
}

SandboxRehearsal Failed(std::string reason, std::filesystem::path root)
{
    SandboxRehearsal result;
    result.supported = true;
    result.prepared = false;
    result.reason = std::move(reason);
    result.root = std::move(root);
    return result;
}

// Lexical containment only. The policy layer does the canonical check with reparse-point
// handling; this decides which root a path belongs under so it can be rewritten.
bool IsUnder(const std::filesystem::path& candidate, const std::filesystem::path& root)
{
    const std::filesystem::path normalizedRoot = root.lexically_normal();
    const std::filesystem::path normalized = candidate.lexically_normal();
    const auto relative = normalized.lexically_relative(normalizedRoot);
    return !relative.empty() && *relative.begin() != "..";
}

// Maps a real path onto its scratch equivalent, preserving the position under the root so
// a plan's relative structure survives.
bool Rewrite(
    std::filesystem::path& value,
    const std::vector<std::filesystem::path>& realRoots,
    const std::filesystem::path& sandboxRoot,
    std::string& outError)
{
    if (value.empty())
    {
        return true;
    }
    for (std::size_t index = 0; index < realRoots.size(); ++index)
    {
        if (!IsUnder(value, realRoots[index]))
        {
            continue;
        }
        const std::filesystem::path relative =
            value.lexically_normal().lexically_relative(realRoots[index].lexically_normal());
        std::filesystem::path mapped = sandboxRoot / ("root" + std::to_string(index));
        if (relative != ".")
        {
            mapped /= relative;
        }
        value = mapped.lexically_normal();
        return true;
    }
    outError = "the path " + value.string() + " is not inside any approved root";
    return false;
}

// Stages the file or directory a step reads from, so a rehearsal observes real content
// rather than an empty tree. A missing source is left missing on purpose: the rehearsal
// should fail exactly where the real run would.
void StageSource(
    const std::filesystem::path& realPath,
    const std::filesystem::path& sandboxPath)
{
    std::error_code error;
    if (!std::filesystem::exists(realPath, error))
    {
        std::filesystem::create_directories(sandboxPath.parent_path(), error);
        return;
    }
    if (std::filesystem::is_directory(realPath, error))
    {
        std::filesystem::create_directories(sandboxPath, error);
        // One level only. A rehearsal needs the directory to look plausible to a listing,
        // not to be a full recursive duplicate of somewhere that could be enormous.
        for (const auto& entry : std::filesystem::directory_iterator(realPath, error))
        {
            std::error_code entryError;
            if (entry.is_directory(entryError))
            {
                std::filesystem::create_directories(
                    sandboxPath / entry.path().filename(), entryError);
                continue;
            }
            std::filesystem::copy_file(
                entry.path(),
                sandboxPath / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing,
                entryError);
        }
        return;
    }
    std::filesystem::create_directories(sandboxPath.parent_path(), error);
    std::filesystem::copy_file(
        realPath, sandboxPath, std::filesystem::copy_options::overwrite_existing, error);
}

} // namespace

bool GoalSandbox::IsFilesystemAction(const actions::ActionType type)
{
    switch (type)
    {
        case actions::ActionType::ListDirectory:
        case actions::ActionType::ReadTextFile:
        case actions::ActionType::CreateDirectory:
        case actions::ActionType::CopyFile:
        case actions::ActionType::MoveFile:
        case actions::ActionType::RenamePath:
        case actions::ActionType::MoveToRecycleBin:
            return true;
        default:
            return false;
    }
}

SandboxRehearsal GoalSandbox::Prepare(const Goal& goal)
{
    if (goal.steps.empty())
    {
        return Unsupported("the plan has no steps");
    }
    for (const GoalStep& step : goal.steps)
    {
        if (!IsFilesystemAction(step.action.type) || !IsFilesystemAction(step.check.type))
        {
            return Unsupported(
                "the plan drives an application, which cannot be copied into a scratch "
                "directory");
        }
    }
    if (goal.scope.approvedRoots.empty())
    {
        return Unsupported("the goal scope has no approved root to mirror");
    }

    std::error_code error;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path(error) /
        "Revia" / "GoalSandbox" / (goal.id + "-" + std::to_string(stamp));
    if (error)
    {
        return Failed("the temporary directory could not be located", {});
    }
    std::filesystem::create_directories(root, error);
    if (error)
    {
        return Failed("the scratch directory could not be created: " + error.message(), root);
    }

    SandboxRehearsal result;
    result.supported = true;
    result.root = root;
    result.goal = goal;
    // A rehearsal is a separate run: it gets its own identity and a clean budget, so what
    // it spends is never charged against the real attempt.
    result.goal.id = NewGoalId();
    result.goal.status = GoalStatus::Planned;
    result.goal.stopReason = StopReason::None;
    result.goal.currentStep = 0;
    result.goal.spend = GoalSpend{};

    std::vector<std::filesystem::path> sandboxRoots;
    for (std::size_t index = 0; index < goal.scope.approvedRoots.size(); ++index)
    {
        const std::filesystem::path mirrored = root / ("root" + std::to_string(index));
        std::filesystem::create_directories(mirrored, error);
        if (error)
        {
            return Failed("a mirrored root could not be created: " + error.message(), root);
        }
        sandboxRoots.push_back(mirrored);
    }

    for (GoalStep& step : result.goal.steps)
    {
        step.id = NewStepId();
        step.status = StepStatus::Pending;
        step.attempts.clear();

        // Stage before rewriting, while the request still names the real location.
        for (const actions::ActionRequest* request : {&step.action, &step.check})
        {
            if (request->source.empty())
            {
                continue;
            }
            std::filesystem::path staged = request->source;
            std::string ignored;
            if (Rewrite(staged, goal.scope.approvedRoots, root, ignored))
            {
                StageSource(request->source, staged);
            }
        }

        for (actions::ActionRequest* request : {&step.action, &step.check})
        {
            request->id = actions::NewActionId();
            std::string rewriteError;
            if (!Rewrite(request->source, goal.scope.approvedRoots, root, rewriteError) ||
                !Rewrite(request->destination, goal.scope.approvedRoots, root, rewriteError))
            {
                return Failed(
                    "step " + std::to_string(step.ordinal + 1) + " cannot be rehearsed: " +
                        rewriteError,
                    root);
            }
        }
    }

    result.goal.scope.approvedRoots = sandboxRoots;
    result.goal.scope.createMissingApprovedRoots = false;

    nlohmann::json approvedRootsJson = nlohmann::json::array();
    for (const std::filesystem::path& mirrored : sandboxRoots)
    {
        approvedRootsJson.push_back(actions::PathToUtf8(mirrored));
    }
    // approved_scope with no approved applications: the rehearsal can reach the scratch
    // tree and nothing else, and cannot drive an application even if a step tried to. The
    // risk ceiling is copied from the goal rather than fixed, so a rehearsal is never more
    // permissive than the run it is standing in for -- otherwise a plan could clear
    // rehearsal and then stall on confirmations it never had to face.
    const nlohmann::json capabilities = {
        {"mode", "approved_scope"},
        {"approvedRoots", approvedRootsJson},
        {"approvedApplications", nlohmann::json::array()},
        {"autoApproveRiskThrough", actions::ToString(goal.scope.autoApproveRiskThrough)},
        {"createMissingApprovedRoots", false},
        {"maxReadBytes", goal.scope.maxReadBytes},
        {"maxDirectoryEntries", goal.scope.maxDirectoryEntries},
        {"maxAffectedEntries", goal.scope.maxAffectedEntries}
    };
    result.capabilityConfig = root / "capabilities.json";
    {
        std::ofstream stream(result.capabilityConfig, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return Failed("the rehearsal capability config could not be written", root);
        }
        stream << capabilities.dump(2);
        if (!stream.good())
        {
            return Failed("the rehearsal capability config could not be written", root);
        }
    }
    result.auditLog = root / "rehearsal-audit.jsonl";

    result.prepared = true;
    return result;
}

bool GoalSandbox::Discard(const std::filesystem::path& root, std::string& outError)
{
    if (root.empty())
    {
        return true;
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (error)
    {
        outError = error.message();
        return false;
    }
    return true;
}

} // namespace revia::goals
