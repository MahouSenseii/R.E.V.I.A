#include "Policy/capabilityProjection.h"

#include <stdexcept>

void RunCapabilityProjectionTests()
{
    using namespace revia;
    actions::CapabilitySettings captured;
    captured.approvedRoots = {"C:/work"};
    captured.approvedApplications = {"editor.exe", "removed.exe"};
    captured.approvedControls["editor.exe"] = {"*"};
    captured.process.enabled = captured.process.allowCommandInterpreters = true;
    captured.process.approvedExecutables = {"C:/tools/build.exe", "C:/tools/removed.exe"};
    captured.process.approvedEnvironmentNames = {"BUILD_MODE", "OLD"};
    captured.desktopControl.pointer = captured.desktopControl.keyboard = true;
    captured.internet.enabled = true;
    auto current = captured;
    current.approvedRoots = {"C:/work/project", "C:/unrelated"};
    current.approvedApplications = {"EDITOR.EXE", "new.exe"};
    current.approvedControls.clear();
    current.approvedControls["EDITOR.EXE"] = {"Save"};
    current.process.approvedExecutables = {"C:/tools/build.exe", "C:/tools/new.exe"};
    current.process.approvedEnvironmentNames = {"BUILD_MODE", "NEW"};
    current.process.allowCommandInterpreters = false;
    current.process.maxOutputBytes = 4096;
    current.desktopControl.keyboard = false;
    current.internet.enabled = false;
    const auto scope = policy::ProjectCapabilityScope(captured, current);
    if (scope.approvedRoots != std::vector<std::filesystem::path>{"C:/work/project"} || scope.approvedApplications.size() != 1 ||
        scope.approvedControls.at("editor.exe") != std::vector<std::string>{"Save"} || scope.process.approvedExecutables.size() != 1 ||
        scope.process.approvedEnvironmentNames != std::vector<std::string>{"BUILD_MODE"} || scope.process.allowCommandInterpreters ||
        scope.process.maxOutputBytes != 4096 || scope.desktopControl.keyboard || scope.internet.enabled)
        throw std::runtime_error("Tool projection retained withdrawn authority or gained a new grant.");
    current.mode = actions::ExecutionMode::Disabled;
    if (policy::ProjectCapabilityScope(captured, current).mode != actions::ExecutionMode::Disabled)
        throw std::runtime_error("Tool projection ignored disabled execution.");
}
