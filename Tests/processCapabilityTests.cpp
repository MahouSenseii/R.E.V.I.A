#include "Policy/permissionStore.h"
#include "Policy/capabilityEditor.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace
{
void Require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class ConfigurationFile
{
public:
    ConfigurationFile() : path(std::filesystem::temp_directory_path() / (revia::actions::NewActionId() + ".json")) {}
    ~ConfigurationFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    bool Load(const std::string& content, revia::actions::CapabilitySettings& settings, std::string& error) const
    {
        auto data = nlohmann::json::parse(content);
        data["approvedRoots"] = {revia::actions::PathToUtf8(std::filesystem::temp_directory_path())};
        std::ofstream(path) << data;
        return revia::policy::PermissionStore().Load(path, settings, error);
    }
    void VerifyBrowserEdit() const
    {
        revia::browser::BrowserSettings browser;
        browser.enabled = true;
        browser.navigate = true;
        browser.approvedOrigins = {"https://example.com"};
        std::string error;
        Require(revia::policy::CapabilityEditor().SetInteractiveBrowser(path, browser, error),
            "The browser editor could not replace the settings file after reading it.");
        revia::actions::CapabilitySettings saved;
        Require(revia::policy::PermissionStore().Load(path, saved, error) && saved.browser.enabled &&
                saved.browser.approvedOrigins == browser.approvedOrigins,
            "Browser permissions were not persisted and reloaded.");
    }
private:
    std::filesystem::path path;
};
}

void RunProcessCapabilityTests()
{
    ConfigurationFile file;
    revia::actions::CapabilitySettings settings;
    std::string error;
    Require(!file.Load(R"({"process":{"enabled":"yes"}})", settings, error),
        "Malformed process authority was silently ignored.");
    Require(!file.Load(R"({"process":{"enabled":true,"approvedExecutables":["relative.exe"]}})", settings, error),
        "A relative executable silently established process authority.");
    Require(!file.Load(R"({"process":{"maxTimeoutMs":600001}})", settings, error),
        "An invalid process timeout was silently accepted.");
    Require(!file.Load(R"({"process":{"maxOutputBytes":0}})", settings, error),
        "A zero process output budget was accepted.");
    Require(!file.Load(R"({"process":{"approvedEnvironmentNames":["BAD=NAME"]}})", settings, error),
        "An invalid environment name was accepted.");
    Require(file.Load("{}", settings, error), "Legacy capability configuration stopped loading.");
    Require(!file.Load(R"({"browser":{"enabled":true,"approvedOrigins":["https://example.com/path"]}})", settings, error),
        "An origin grant accepted a path instead of an exact origin.");
    Require(!file.Load(R"({"browser":{"maxElements":0}})", settings, error), "An invalid browser observation budget was accepted.");
    Require(!file.Load(R"({"browser":{"interact":"yes"}})", settings, error), "Malformed browser interaction authority was accepted.");
    Require(file.Load("{}", settings, error), "Could not restore a valid policy for the editor check.");
    file.VerifyBrowserEdit();
}
