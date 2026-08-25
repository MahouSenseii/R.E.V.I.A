#include "Policy/permissionStore.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace revia::policy
{

using json = nlohmann::json;

namespace
{

bool IsSupportedMode(const std::string& value)
{
    return value == "disabled" || value == "supervised" ||
        value == "approved_scope" || value == "autonomous";
}

bool IsSupportedRisk(const std::string& value)
{
    return value == "read" || value == "read_only" || value == "write" ||
        value == "reversible" || value == "reversible_write" ||
        value == "destructive";
}

bool IsSafeHost(const std::string& value)
{
    if (value.empty() || value.size() > 253 || value.front() == '.' || value.back() == '.')
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char character)
    {
        return std::isalnum(character) || character == '.' || character == '-';
    });
}

template <typename T>
T BoundedInteger(const json& data, const char* key, T fallback, T minimum, T maximum)
{
    if (!data.contains(key) || !data[key].is_number_integer())
    {
        return fallback;
    }

    const auto raw = data[key].get<long long>();
    if (raw < static_cast<long long>(minimum) || raw > static_cast<long long>(maximum))
    {
        return fallback;
    }
    return static_cast<T>(raw);
}

} // namespace

bool PermissionStore::Load(
    const std::filesystem::path& path,
    actions::CapabilitySettings& outSettings,
    std::string& outError) const
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        outError = "Could not open capability configuration: " + actions::PathToUtf8(path);
        return false;
    }

    try
    {
        json data;
        file >> data;
        if (!data.is_object())
        {
            outError = "Capability configuration must be a JSON object.";
            return false;
        }

        const std::string modeName = data.value("mode", "supervised");
        if (!IsSupportedMode(modeName))
        {
            outError = "Unsupported capability mode: " + modeName;
            return false;
        }
        const std::string riskName = data.value("autoApproveRiskThrough", "read_only");
        if (!IsSupportedRisk(riskName))
        {
            outError = "Unsupported automatic risk ceiling: " + riskName;
            return false;
        }

        actions::CapabilitySettings settings;
        settings.mode = actions::ExecutionModeFromString(modeName);
        settings.autoApproveRiskThrough = actions::RiskLevelFromString(riskName);
        settings.createMissingApprovedRoots = data.value("createMissingApprovedRoots", true);
        settings.maxReadBytes = BoundedInteger<std::uintmax_t>(
            data, "maxReadBytes", 1024U * 1024U, 1024U, 64U * 1024U * 1024U);
        settings.maxDirectoryEntries = BoundedInteger<std::size_t>(
            data, "maxDirectoryEntries", 500U, 1U, 10000U);
        settings.maxAffectedEntries = BoundedInteger<std::size_t>(
            data, "maxAffectedEntries", 200U, 1U, 100000U);
        settings.maxDesktopActionsPerMinute = BoundedInteger<int>(
            data, "maxDesktopActionsPerMinute", 12, 1, 600);
        settings.minimumDesktopActionIntervalMs = BoundedInteger<int>(
            data, "minimumDesktopActionIntervalMs", 250, 0, 60000);

        if (data.contains("internet"))
        {
            const json& internet = data["internet"];
            if (!internet.is_object())
            {
                outError = "internet must be an object.";
                return false;
            }
            settings.internet.enabled = internet.value("enabled", false);
            settings.internet.automaticLookup = internet.value("automaticLookup", true);
            settings.internet.provider = internet.value("provider", "duckduckgo");
            settings.internet.requestTimeoutMs = BoundedInteger<int>(
                internet, "requestTimeoutMs", 8000, 1000, 30000);
            settings.internet.maxResponseBytes = BoundedInteger<std::size_t>(
                internet, "maxResponseBytes", 256U * 1024U, 4096U, 2U * 1024U * 1024U);
            settings.internet.maxRequestsPerMinute = BoundedInteger<int>(
                internet, "maxRequestsPerMinute", 12, 1, 120);
            settings.internet.maxResults = BoundedInteger<int>(
                internet, "maxResults", 5, 1, 10);
            settings.internet.visibleBrowser = internet.value("visibleBrowser", false);
            settings.internet.autonomousResearch =
                internet.value("autonomousResearch", false);
            settings.internet.visibleBrowserPort = BoundedInteger<int>(
                internet, "visibleBrowserPort", 8095, 1024, 65535);
            settings.internet.visibleBrowserStartupTimeoutMs = BoundedInteger<int>(
                internet, "visibleBrowserStartupTimeoutMs", 8000, 1000, 30000);
            settings.internet.visibleBrowserRequestTimeoutMs = BoundedInteger<int>(
                internet, "visibleBrowserRequestTimeoutMs", 30000, 5000, 120000);
            settings.internet.visibleBrowserMaxPages = BoundedInteger<int>(
                internet, "visibleBrowserMaxPages", 3, 1, 5);
            settings.internet.visibleBrowserStepDelayMs = BoundedInteger<int>(
                internet, "visibleBrowserStepDelayMs", 250, 0, 3000);
            if (settings.internet.autonomousResearch &&
                (!settings.internet.enabled || !settings.internet.visibleBrowser))
            {
                outError = "Autonomous internet research requires enabled visible browsing.";
                return false;
            }
            settings.internet.approvedHosts.clear();
            if (!internet.contains("approvedHosts") || !internet["approvedHosts"].is_array())
            {
                outError = "internet.approvedHosts must be an array.";
                return false;
            }
            for (const auto& host : internet["approvedHosts"])
            {
                if (!host.is_string() || !IsSafeHost(host.get<std::string>()))
                {
                    outError = "Every approved internet host must be a plain DNS name.";
                    return false;
                }
                settings.internet.approvedHosts.push_back(host.get<std::string>());
            }
            if (settings.internet.provider != "duckduckgo")
            {
                outError = "Unsupported internet search provider: " +
                    settings.internet.provider;
                return false;
            }
            const bool providerAllowed = std::any_of(
                settings.internet.approvedHosts.begin(),
                settings.internet.approvedHosts.end(),
                [](const std::string& host)
                {
                    std::string lowered = host;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                        [](const unsigned char c)
                        {
                            return static_cast<char>(std::tolower(c));
                        });
                    return lowered == "api.duckduckgo.com";
                });
            if (settings.internet.enabled && !providerAllowed)
            {
                outError = "Enabled DuckDuckGo lookup requires api.duckduckgo.com in approvedHosts.";
                return false;
            }
        }

        if (!data.contains("approvedRoots") || !data["approvedRoots"].is_array())
        {
            outError = "Capability configuration requires an approvedRoots array.";
            return false;
        }

        for (const auto& root : data["approvedRoots"])
        {
            if (!root.is_string())
            {
                outError = "Every approved root must be a string.";
                return false;
            }
            const std::string expanded = ExpandEnvironmentVariables(root.get<std::string>());
            if (expanded.empty())
            {
                outError = "An approved root expanded to an empty path.";
                return false;
            }
            settings.approvedRoots.emplace_back(actions::Utf8ToPath(expanded));
        }

        if (data.contains("approvedApplications"))
        {
            if (!data["approvedApplications"].is_array())
            {
                outError = "approvedApplications must be an array.";
                return false;
            }
            for (const auto& application : data["approvedApplications"])
            {
                if (!application.is_string() || application.get<std::string>().empty())
                {
                    outError = "Every approved application must be a non-empty executable name.";
                    return false;
                }
                settings.approvedApplications.push_back(application.get<std::string>());
            }
        }

        if (data.contains("approvedControls"))
        {
            if (!data["approvedControls"].is_object())
            {
                outError = "approvedControls must be an object keyed by executable name.";
                return false;
            }
            for (auto entry = data["approvedControls"].begin();
                 entry != data["approvedControls"].end(); ++entry)
            {
                if (entry.key().empty() || !entry.value().is_array())
                {
                    outError = "Each approvedControls entry requires a control array.";
                    return false;
                }
                std::vector<std::string> controls;
                for (const auto& control : entry.value())
                {
                    if (!control.is_string() || control.get<std::string>().empty())
                    {
                        outError = "Every approved control must be a non-empty string.";
                        return false;
                    }
                    controls.push_back(control.get<std::string>());
                }
                settings.approvedControls.emplace(entry.key(), std::move(controls));
            }
        }

        for (const std::string& application : settings.approvedApplications)
        {
            const std::string loweredApplication = [&application]()
            {
                std::string value = application;
                std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });
                return value;
            }();
            const bool hasControlScope = std::any_of(
                settings.approvedControls.begin(),
                settings.approvedControls.end(),
                [&loweredApplication](const auto& entry)
                {
                    std::string key = entry.first;
                    std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char c)
                    {
                        return static_cast<char>(std::tolower(c));
                    });
                    return key == loweredApplication;
                });
            if (!hasControlScope)
            {
                outError = "Every approved application requires an approvedControls entry.";
                return false;
            }
        }

        if (settings.approvedRoots.empty())
        {
            outError = "At least one approved root is required.";
            return false;
        }

        outSettings = std::move(settings);
        outError.clear();
        return true;
    }
    catch (const std::exception& error)
    {
        outError = std::string("Invalid capability configuration: ") + error.what();
        return false;
    }
}

std::string PermissionStore::ExpandEnvironmentVariables(const std::string& value)
{
    std::string expanded;
    expanded.reserve(value.size());

    for (std::size_t index = 0; index < value.size();)
    {
        if (value[index] != '%')
        {
            expanded.push_back(value[index++]);
            continue;
        }

        const std::size_t end = value.find('%', index + 1);
        if (end == std::string::npos)
        {
            expanded.append(value.substr(index));
            break;
        }

        const std::string name = value.substr(index + 1, end - index - 1);
        const char* environmentValue = std::getenv(name.c_str());
        if (environmentValue == nullptr)
        {
            expanded.append(value.substr(index, end - index + 1));
        }
        else
        {
            expanded.append(environmentValue);
        }
        index = end + 1;
    }

    return expanded;
}

} // namespace revia::policy
