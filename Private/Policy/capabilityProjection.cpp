#include "Policy/capabilityProjection.h"

#include <algorithm>
#include <cctype>

namespace revia::policy
{
namespace
{
std::string Fold(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string Key(const std::filesystem::path& path)
{
    const auto text = path.lexically_normal().generic_u8string();
    return Fold(std::string(text.begin(), text.end()));
}

bool Within(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    auto root = Key(parent);
    const auto candidate = Key(child);
    if (candidate == root)
        return true;
    if (root.empty())
        return false;
    if (root.back() != '/')
        root += '/';
    return candidate.starts_with(root);
}

std::vector<std::string> Common(const std::vector<std::string>& first, const std::vector<std::string>& second, bool wildcard = false)
{
    const auto has = [](const auto& list, const std::string& value)
    { return std::any_of(list.begin(), list.end(), [&](const auto& item) { return Fold(item) == Fold(value); }); };
    if (wildcard && has(first, "*"))
        return second;
    if (wildcard && has(second, "*"))
        return first;
    std::vector<std::string> result;
    for (const auto& value : first)
        if (has(second, value))
            result.push_back(value);
    return result;
}
}

actions::CapabilitySettings ProjectCapabilityScope(const actions::CapabilitySettings& captured, const actions::CapabilitySettings& current)
{
    auto result = captured;
    using Mode = actions::ExecutionMode;
    if (captured.mode == Mode::Disabled || current.mode == Mode::Disabled)
        result.mode = Mode::Disabled;
    else if (captured.mode == Mode::Supervised || current.mode == Mode::Supervised)
        result.mode = Mode::Supervised;
    else if (captured.mode == Mode::ApprovedScope || current.mode == Mode::ApprovedScope)
        result.mode = Mode::ApprovedScope;
    result.autoApproveRiskThrough = std::min(captured.autoApproveRiskThrough, current.autoApproveRiskThrough);
    result.approvedRoots.clear();
    for (const auto& left : captured.approvedRoots)
        for (const auto& right : current.approvedRoots)
        {
            if (Within(left, right))
                result.approvedRoots.push_back(left);
            else if (Within(right, left))
                result.approvedRoots.push_back(right);
        }
    result.approvedApplications = Common(captured.approvedApplications, current.approvedApplications);
    result.approvedControls.clear();
    for (const auto& app : result.approvedApplications)
    {
        std::vector<std::string> left, right;
        for (const auto& [name, controls] : captured.approvedControls)
            if (Fold(name) == Fold(app))
                left = controls;
        for (const auto& [name, controls] : current.approvedControls)
            if (Fold(name) == Fold(app))
                right = controls;
        result.approvedControls[app] = Common(left, right, true);
    }
    result.maxReadBytes = std::min(captured.maxReadBytes, current.maxReadBytes);
    result.maxDirectoryEntries = std::min(captured.maxDirectoryEntries, current.maxDirectoryEntries);
    result.maxAffectedEntries = std::min(captured.maxAffectedEntries, current.maxAffectedEntries);
    result.maxDesktopActionsPerMinute = std::min(captured.maxDesktopActionsPerMinute, current.maxDesktopActionsPerMinute);
    result.minimumDesktopActionIntervalMs = std::max(captured.minimumDesktopActionIntervalMs, current.minimumDesktopActionIntervalMs);
    auto& desktop = result.desktopControl;
    const auto& now = current.desktopControl;
    desktop.pointer &= now.pointer;
    desktop.keyboard &= now.keyboard;
    desktop.applicationLaunch &= now.applicationLaunch;
    desktop.rawCoordinates &= now.rawCoordinates;
    desktop.visualTargeting &= now.visualTargeting;
    desktop.autonomous &= now.autonomous;
    desktop.allowCommandSurfaces &= now.allowCommandSurfaces;
    desktop.scope = std::min(desktop.scope, now.scope);
    desktop.maxUnconfirmedConsequence = std::min(desktop.maxUnconfirmedConsequence, now.maxUnconfirmedConsequence);
    desktop.maxInputActionsPerMinute = std::min(desktop.maxInputActionsPerMinute, now.maxInputActionsPerMinute);
    desktop.minimumInputIntervalMs = std::max(desktop.minimumInputIntervalMs, now.minimumInputIntervalMs);
    desktop.maxTypedCharacters = std::min(desktop.maxTypedCharacters, now.maxTypedCharacters);
    auto& process = result.process;
    process.allowTaskExecution &= current.process.allowTaskExecution;
    process.enabled &= current.process.enabled;
    process.allowCommandInterpreters &= current.process.allowCommandInterpreters;
    std::erase_if(process.approvedExecutables,
        [&](const auto& path)
        {
            return std::none_of(current.process.approvedExecutables.begin(), current.process.approvedExecutables.end(),
                [&](const auto& approved) { return Key(path) == Key(approved); });
        });
    process.approvedEnvironmentNames = Common(process.approvedEnvironmentNames, current.process.approvedEnvironmentNames);
    process.maxTimeoutMs = std::min(process.maxTimeoutMs, current.process.maxTimeoutMs);
    process.maxOutputBytes = std::min(process.maxOutputBytes, current.process.maxOutputBytes);
    process.maxEnvironmentBytes = std::min(process.maxEnvironmentBytes, current.process.maxEnvironmentBytes);
    auto& internet = result.internet;
    internet.enabled &= current.internet.enabled && internet.provider == current.internet.provider;
    internet.automaticLookup &= current.internet.automaticLookup;
    internet.visibleBrowser &= current.internet.visibleBrowser;
    internet.autonomousResearch &= current.internet.autonomousResearch;
    internet.approvedHosts = Common(internet.approvedHosts, current.internet.approvedHosts);
    internet.requestTimeoutMs = std::min(internet.requestTimeoutMs, current.internet.requestTimeoutMs);
    internet.maxResponseBytes = std::min(internet.maxResponseBytes, current.internet.maxResponseBytes);
    internet.maxRequestsPerMinute = std::min(internet.maxRequestsPerMinute, current.internet.maxRequestsPerMinute);
    internet.maxResults = std::min(internet.maxResults, current.internet.maxResults);
    internet.visibleBrowserMaxPages = std::min(internet.visibleBrowserMaxPages, current.internet.visibleBrowserMaxPages);
    result.camera.enabled &= current.camera.enabled;
    result.image.enabled &= current.image.enabled && Key(result.image.outputRoot) == Key(current.image.outputRoot);
    result.image.autonomous &= current.image.autonomous;
    result.camera.autonomousCapture &= current.camera.autonomousCapture;
    result.browser = browser::IntersectSettings(captured.browser, current.browser);
    return result;
}
}
