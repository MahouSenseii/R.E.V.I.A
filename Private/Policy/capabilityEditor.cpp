#include "Policy/capabilityEditor.h"

#include "Actions/actionTypes.h"
#include "Policy/permissionStore.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::policy
{

namespace
{
using json = nlohmann::json;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ValidExecutable(const std::string& value)
{
    return !value.empty() && value.size() <= 260 &&
        value.find_first_of("/\\") == std::string::npos &&
        Lower(value).ends_with(".exe");
}

json::iterator FindApplication(json& applications, const std::string& executable)
{
    const std::string wanted = Lower(executable);
    return std::find_if(applications.begin(), applications.end(), [&](const json& entry)
    {
        return entry.is_string() && Lower(entry.get<std::string>()) == wanted;
    });
}

json::iterator FindControlScope(json& controls, const std::string& executable)
{
    const std::string wanted = Lower(executable);
    for (auto entry = controls.begin(); entry != controls.end(); ++entry)
    {
        if (Lower(entry.key()) == wanted)
        {
            return entry;
        }
    }
    return controls.end();
}

bool ReplaceValidated(
    const std::filesystem::path& path,
    const json& data,
    std::string& outError)
{
    std::filesystem::path temporary = path;
    temporary += ".pending";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            outError = "Could not create the pending capability configuration.";
            return false;
        }
        stream << data.dump(2) << '\n';
        if (!stream.good())
        {
            outError = "Could not finish writing the pending capability configuration.";
            return false;
        }
    }

    PermissionStore validator;
    actions::CapabilitySettings ignored;
    if (!validator.Load(temporary, ignored, outError))
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }

#ifdef _WIN32
    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        outError = "Windows could not atomically replace the capability configuration.";
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        outError = "Could not replace the capability configuration: " + error.message();
        std::filesystem::remove(temporary, error);
        return false;
    }
#endif
    outError.clear();
    return true;
}
}

bool CapabilityEditor::AddApplication(
    const std::filesystem::path& path,
    const std::string& executable,
    std::string& outError) const
{
    return Apply(path, Mutation::AddApplication, executable, {}, false, false, outError);
}

bool CapabilityEditor::RemoveApplication(
    const std::filesystem::path& path,
    const std::string& executable,
    std::string& outError) const
{
    return Apply(path, Mutation::RemoveApplication, executable, {}, false, false, outError);
}

bool CapabilityEditor::AddControl(
    const std::filesystem::path& path,
    const std::string& executable,
    const std::string& control,
    std::string& outError) const
{
    return Apply(path, Mutation::AddControl, executable, control, false, false, outError);
}

bool CapabilityEditor::RemoveControl(
    const std::filesystem::path& path,
    const std::string& executable,
    const std::string& control,
    std::string& outError) const
{
    return Apply(path, Mutation::RemoveControl, executable, control, false, false, outError);
}

bool CapabilityEditor::SetInternetAccess(
    const std::filesystem::path& path,
    const bool enabled,
    const bool automaticLookup,
    std::string& outError) const
{
    return Apply(path, Mutation::Internet, {}, {}, enabled, automaticLookup, outError);
}

bool CapabilityEditor::SetInternetBrowser(
    const std::filesystem::path& path,
    const bool visibleBrowser,
    const bool autonomousResearch,
    std::string& outError) const
{
    return Apply(
        path, Mutation::Browser, {}, {}, visibleBrowser, autonomousResearch, outError);
}

bool CapabilityEditor::Apply(
    const std::filesystem::path& path,
    const Mutation mutation,
    const std::string& executable,
    const std::string& control,
    const bool enabled,
    const bool automaticLookup,
    std::string& outError) const
{
    if (mutation != Mutation::Internet && mutation != Mutation::Browser &&
        !ValidExecutable(executable))
    {
        outError = "An application permission requires a plain .exe name.";
        return false;
    }
    if ((mutation == Mutation::AddControl || mutation == Mutation::RemoveControl) &&
        (control.empty() || control.size() > 512))
    {
        outError = "A control permission requires a non-empty accessible name or automation id.";
        return false;
    }

    json data;
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            outError = "Could not open the capability configuration.";
            return false;
        }
        stream >> data;
    }
    catch (const std::exception& error)
    {
        outError = std::string("Could not parse the capability configuration: ") + error.what();
        return false;
    }

    json& applications = data["approvedApplications"];
    json& controls = data["approvedControls"];
    if (!applications.is_array() || !controls.is_object())
    {
        outError = "The capability configuration has invalid application permission fields.";
        return false;
    }

    if (mutation == Mutation::AddApplication)
    {
        if (FindApplication(applications, executable) == applications.end())
        {
            applications.push_back(executable);
        }
        if (FindControlScope(controls, executable) == controls.end())
        {
            controls[executable] = json::array();
        }
    }
    else if (mutation == Mutation::RemoveApplication)
    {
        const auto found = FindApplication(applications, executable);
        if (found != applications.end())
        {
            applications.erase(found);
        }
        const auto scope = FindControlScope(controls, executable);
        if (scope != controls.end())
        {
            controls.erase(scope);
        }
    }
    else if (mutation == Mutation::AddControl || mutation == Mutation::RemoveControl)
    {
        if (FindApplication(applications, executable) == applications.end())
        {
            outError = "Approve the application before changing one of its controls.";
            return false;
        }
        auto scope = FindControlScope(controls, executable);
        if (scope == controls.end())
        {
            controls[executable] = json::array();
            scope = FindControlScope(controls, executable);
        }
        json& list = scope.value();
        const std::string wanted = Lower(control);
        const auto found = std::find_if(list.begin(), list.end(), [&](const json& entry)
        {
            return entry.is_string() && Lower(entry.get<std::string>()) == wanted;
        });
        if (mutation == Mutation::AddControl && found == list.end())
        {
            list.push_back(control);
        }
        else if (mutation == Mutation::RemoveControl && found != list.end())
        {
            list.erase(found);
        }
    }
    else
    {
        json& internet = data["internet"];
        if (!internet.is_object())
        {
            internet = json::object();
        }
        if (mutation == Mutation::Internet)
        {
            internet["enabled"] = enabled;
            internet["automaticLookup"] = automaticLookup;
            if (!enabled) internet["autonomousResearch"] = false;
        }
        else
        {
            internet["visibleBrowser"] = enabled;
            internet["autonomousResearch"] = enabled && automaticLookup;
            if (automaticLookup) internet["enabled"] = true;
        }
        if (!internet.contains("provider")) internet["provider"] = "duckduckgo";
        if (!internet.contains("approvedHosts"))
        {
            internet["approvedHosts"] = {"api.duckduckgo.com", "en.wikipedia.org"};
        }
        if (!internet.contains("requestTimeoutMs")) internet["requestTimeoutMs"] = 8000;
        if (!internet.contains("maxResponseBytes")) internet["maxResponseBytes"] = 262144;
        if (!internet.contains("maxRequestsPerMinute")) internet["maxRequestsPerMinute"] = 12;
        if (!internet.contains("maxResults")) internet["maxResults"] = 5;
        if (!internet.contains("visibleBrowser")) internet["visibleBrowser"] = false;
        if (!internet.contains("autonomousResearch")) internet["autonomousResearch"] = false;
        if (!internet.contains("visibleBrowserPort")) internet["visibleBrowserPort"] = 8095;
        if (!internet.contains("visibleBrowserStartupTimeoutMs"))
            internet["visibleBrowserStartupTimeoutMs"] = 8000;
        if (!internet.contains("visibleBrowserRequestTimeoutMs"))
            internet["visibleBrowserRequestTimeoutMs"] = 30000;
        if (!internet.contains("visibleBrowserMaxPages"))
            internet["visibleBrowserMaxPages"] = 3;
        if (!internet.contains("visibleBrowserStepDelayMs"))
            internet["visibleBrowserStepDelayMs"] = 250;
    }

    return ReplaceValidated(path, data, outError);
}

} // namespace revia::policy
