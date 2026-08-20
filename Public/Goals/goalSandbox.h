#pragma once

#include "Goals/goalTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace revia::goals
{

struct SandboxRehearsal
{
    // False when the plan cannot be rehearsed at all rather than when rehearsal failed.
    // A plan that drives a real application has nothing to copy into a scratch directory,
    // so it goes to confirmation without evidence and the user is told that.
    bool supported = false;
    bool prepared = false;
    Goal goal;
    std::filesystem::path root;
    // A rehearsal needs its own ActionRuntime, not the session's. Scoped execution takes
    // the more restrictive of the global policy and the goal's, and the scratch directory
    // is outside every configured approved root, so the real runtime would block every
    // step of a plan that is perfectly fine. These two files give the rehearsal a policy
    // that approves the scratch directory and nothing else, plus an audit log that is
    // discarded with it rather than mixed into the real trail.
    std::filesystem::path capabilityConfig;
    std::filesystem::path auditLog;
    // Desktop goals use disposable application windows created after the scratch tree is
    // prepared. Only explicitly supported fixture applications may appear here.
    std::vector<std::string> desktopApplications;
    std::string reason;
};

// Builds a throwaway copy of just the paths a plan touches, so the plan can be proven
// before it is aimed at real folders.
//
// The whole approved root is deliberately not mirrored. A goal's root can be an entire
// documents folder, and copying it to rehearse a two-step plan would cost more than the
// plan does. Only the sources the plan names are staged, and the rehearsal scope is
// narrowed to the scratch directory, so a step that reaches outside what it declared
// fails here instead of succeeding here and surprising someone later.
class GoalSandbox
{
public:
    [[nodiscard]] static SandboxRehearsal Prepare(const Goal& goal);

    // Best effort. A scratch directory that outlives its run is noise, not a hazard, so
    // failure to remove it is reported rather than raised.
    static bool Discard(const std::filesystem::path& root, std::string& outError);

    [[nodiscard]] static bool IsFilesystemAction(actions::ActionType type);
    [[nodiscard]] static bool IsDesktopAction(actions::ActionType type);
};

} // namespace revia::goals
