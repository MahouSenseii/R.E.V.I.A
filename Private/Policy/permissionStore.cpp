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
                if (entry.key().empty() || !entry.value().is_array() || entry.value().empty())
                {
                    outError = "Each approvedControls entry requires a non-empty control array.";
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
