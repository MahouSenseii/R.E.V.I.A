#include "Actions/actionRuntime.h"
#include <algorithm>
#include <cctype>

namespace revia::actions
{
namespace
{
std::string NormalizeExecutableName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}
}

CapabilitySettings ActionRuntime::Settings() const
{
    std::lock_guard lock(settingsMutex);
    auto current = settingsSnapshot;
    for (const auto& [id, reduce] : pendingReductions)
        reduce.apply(current);
    return current;
}

bool ActionRuntime::EditCapabilities(const std::function<bool(std::string&)>& edit, std::function<void(CapabilitySettings&)> reduction,
    const std::string& reductionKey, std::string& outError)
{
    std::uint64_t reductionId;
    const bool publishesReduction = static_cast<bool>(reduction);
    {
        std::lock_guard settingsLock(settingsMutex);
        reductionId = ++nextReductionId;
        if (publishesReduction)
            pendingReductions.emplace(reductionId, TemporaryReduction{std::move(reduction), reductionKey});
    }
    std::lock_guard lock(mutex);
    const bool saved = edit(outError) && ReloadUnlocked(outError);
    if (saved)
    {
        std::lock_guard settingsLock(settingsMutex);
        std::erase_if(pendingReductions,
            [&](const auto& entry)
            {
                return entry.first == reductionId || (entry.first < reductionId && entry.second.failed && entry.second.key == reductionKey);
            });
    }
    else if (publishesReduction)
    {
        std::lock_guard settingsLock(settingsMutex);
        pendingReductions.at(reductionId).failed = true;
        outError += " A restrictive temporary machine ceiling remains active because the change could not be persisted.";
    }
    return saved;
}

bool ActionRuntime::ReloadUnlocked(std::string& outError)
{
    if (capabilityConfigPath.empty() || auditPath.empty())
    {
        outError = "Action runtime has no capability configuration to reload.";
        return false;
    }
    return InitializeUnlocked(capabilityConfigPath, auditPath, outError);
}

bool ActionRuntime::AddApprovedApplication(const std::string& executable, std::string& outError)
{
    return EditCapabilities([&](std::string& error) { return capabilityEditor.AddApplication(capabilityConfigPath, executable, error); },
        {}, "application:" + NormalizeExecutableName(executable), outError);
}

bool ActionRuntime::RemoveApprovedApplication(const std::string& executable, std::string& outError)
{
    return EditCapabilities([&](std::string& error) { return capabilityEditor.RemoveApplication(capabilityConfigPath, executable, error); },
        [executable](CapabilitySettings& settings)
        {
            const auto removed = NormalizeExecutableName(executable);
            std::erase_if(
                settings.approvedApplications, [&](const auto& application) { return NormalizeExecutableName(application) == removed; });
            std::erase_if(settings.approvedControls, [&](const auto& entry) { return NormalizeExecutableName(entry.first) == removed; });
        },
        "application:" + NormalizeExecutableName(executable), outError);
}

bool ActionRuntime::AddApprovedControl(const std::string& executable, const std::string& control, std::string& outError)
{
    return EditCapabilities([&](std::string& error)
        { return capabilityEditor.AddControl(capabilityConfigPath, executable, control, error); }, {},
        "control:" + NormalizeExecutableName(executable) + ":" + NormalizeExecutableName(control), outError);
}

bool ActionRuntime::RemoveApprovedControl(const std::string& executable, const std::string& control, std::string& outError)
{
    return EditCapabilities([&](std::string& error)
        { return capabilityEditor.RemoveControl(capabilityConfigPath, executable, control, error); },
        [executable, control](CapabilitySettings& settings)
        {
            for (auto& [application, controls] : settings.approvedControls)
                if (NormalizeExecutableName(application) == NormalizeExecutableName(executable))
                    std::erase_if(
                        controls, [&](const auto& value) { return NormalizeExecutableName(value) == NormalizeExecutableName(control); });
        },
        "control:" + NormalizeExecutableName(executable) + ":" + NormalizeExecutableName(control), outError);
}

bool ActionRuntime::SetInternetAccess(const bool enabled, const bool automaticLookup, std::string& outError)
{
    return EditCapabilities([&](std::string& error)
        { return capabilityEditor.SetInternetAccess(capabilityConfigPath, enabled, automaticLookup, error); },
        [enabled, automaticLookup](CapabilitySettings& settings)
        {
            settings.internet.enabled = settings.internet.enabled && enabled;
            settings.internet.automaticLookup = settings.internet.automaticLookup && automaticLookup;
            settings.internet.autonomousResearch = settings.internet.autonomousResearch && enabled;
        },
        "internet-access", outError);
}

bool ActionRuntime::SetInternetBrowser(const bool visibleBrowser, const bool autonomousResearch, std::string& outError)
{
    return EditCapabilities([&](std::string& error)
        { return capabilityEditor.SetInternetBrowser(capabilityConfigPath, visibleBrowser, autonomousResearch, error); },
        [visibleBrowser, autonomousResearch](CapabilitySettings& settings)
        {
            settings.internet.visibleBrowser = settings.internet.visibleBrowser && visibleBrowser;
            settings.internet.autonomousResearch = settings.internet.autonomousResearch && visibleBrowser && autonomousResearch;
        },
        "internet-browser", outError);
}

void ActionRuntime::SetDesktopApprovalHandler(policy::DesktopApprovalGate::Handler handler)
{
    // Not under `mutex`: the gate owns its own, and a reload must not be able to
    // block behind a dialog that is waiting on a person.
    desktopApprovals->SetHandler(std::move(handler));
}

policy::DesktopApprovalGate::TaskApproval ActionRuntime::ApproveDesktopTask(const std::string& goalId, const bool messaging)
{
    return desktopApprovals->ApproveTask(goalId, messaging);
}

bool ActionRuntime::SetCameraAccess(const bool enabled, const bool autonomousCapture, std::string& outError)
{
    return EditCapabilities([&](std::string& error)
        { return capabilityEditor.SetCameraAccess(capabilityConfigPath, enabled, autonomousCapture, error); },
        [enabled, autonomousCapture](CapabilitySettings& settings)
        {
            settings.camera.enabled = settings.camera.enabled && enabled;
            settings.camera.autonomousCapture = settings.camera.autonomousCapture && enabled && autonomousCapture;
        },
        "camera-access", outError);
}

bool ActionRuntime::SetProcessSettings(const process::ProcessSettings& settings, std::string& outError)
{
    return EditCapabilities([&](std::string& error) { return capabilityEditor.SetProcessSettings(capabilityConfigPath, settings, error); },
        [](CapabilitySettings& current)
        {
            // Withdraw running command admission before waiting for a configuration reload.
            current.process.enabled = false;
        },
        "process-access", outError);
}

bool ActionRuntime::SetInteractiveBrowser(const browser::BrowserSettings& settings, std::string& outError)
{
    return EditCapabilities([&](std::string& error)
        { return capabilityEditor.SetInteractiveBrowser(capabilityConfigPath, settings, error); },
        [](CapabilitySettings& current) { current.browser.enabled = false; }, "interactive-browser", outError);
}

bool ActionRuntime::SetDesktopControl(const bool pointer, const bool keyboard, const bool applicationLaunch, const bool rawCoordinates,
    const bool visualTargeting, const bool autonomous, const CapabilitySettings::DesktopControl::InputScope scope,
    const bool allowCommandSurfaces, std::string& outError)
{
    return EditCapabilities(
        [&](std::string& error)
        {
            return capabilityEditor.SetDesktopControl(capabilityConfigPath, pointer, keyboard, applicationLaunch, rawCoordinates,
                visualTargeting, autonomous, scope, allowCommandSurfaces, error);
        },
        [=](CapabilitySettings& settings)
        {
            auto& desktop = settings.desktopControl;
            desktop.pointer = desktop.pointer && pointer;
            desktop.keyboard = desktop.keyboard && keyboard;
            desktop.applicationLaunch = desktop.applicationLaunch && applicationLaunch;
            desktop.rawCoordinates = desktop.rawCoordinates && pointer && rawCoordinates;
            desktop.visualTargeting = desktop.visualTargeting && pointer && visualTargeting;
            desktop.autonomous = desktop.autonomous && (pointer || keyboard || applicationLaunch) && autonomous;
            desktop.allowCommandSurfaces = desktop.allowCommandSurfaces && allowCommandSurfaces;
            if (scope == CapabilitySettings::DesktopControl::InputScope::ApprovedApplications)
                desktop.scope = scope;
        },
        "desktop-control", outError);
}

bool ActionRuntime::SetExecutionMode(const ExecutionMode mode, std::string& outError)
{
    return EditCapabilities([&](std::string& error) { return capabilityEditor.SetExecutionMode(capabilityConfigPath, mode, error); },
        [mode](CapabilitySettings& settings)
        {
            const auto rank = [](ExecutionMode value)
            {
                switch (value)
                {
                case ExecutionMode::Disabled:
                    return 3;
                case ExecutionMode::Supervised:
                    return 2;
                case ExecutionMode::ApprovedScope:
                    return 1;
                case ExecutionMode::OwnerFullAccess:
                    return 0;
                }
                return 3;
            };
            if (rank(mode) > rank(settings.mode))
                settings.mode = mode;
        },
        "execution-mode", outError);
}

} // namespace revia::actions
