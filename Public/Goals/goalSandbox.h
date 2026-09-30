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
    // Rehearsal uses a separate runtime restricted to scratch paths and a disposable audit log.
    std::filesystem::path capabilityConfig;
    std::filesystem::path auditLog;
    // Desktop goals use disposable application windows created after the scratch tree is
    // prepared. Only explicitly supported fixture applications may appear here.
    std::vector<std::string> desktopApplications;
    std::string reason;
};

// Stages only named sources, never entire approved roots.
// Rehearsal scope is restricted to the scratch directory.
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
