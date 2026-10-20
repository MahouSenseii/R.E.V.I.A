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
    // Turning the browser or running programs off withdraws the narrower grants inside them, so
    // turning it on again cannot bring a risky one back without its own question.
    void VerifyInnerGrantsAreWithdrawn() const
    {
        const revia::policy::CapabilityEditor editor;
        revia::browser::BrowserSettings browser;
        browser.enabled = true;
        browser.navigate = true;
        browser.interact = true;
        browser.allowTaskInteraction = true;
        browser.allowLoopback = true;
        browser.approvedOrigins = {"https://example.com"};
        std::string error;
        revia::actions::CapabilitySettings saved;
        Require(editor.SetInteractiveBrowser(path, browser, error) && revia::policy::PermissionStore().Load(path, saved, error) &&
                saved.browser.interact && saved.browser.allowTaskInteraction && saved.browser.allowLoopback,
            "The browser's risky grants could not be given.");
        browser.enabled = false;
        Require(editor.SetInteractiveBrowser(path, browser, error) && revia::policy::PermissionStore().Load(path, saved, error) &&
                !saved.browser.enabled && !saved.browser.interact && !saved.browser.allowTaskInteraction && !saved.browser.allowLoopback &&
                saved.browser.navigate,
            "A risky browser grant survived the browser being turned off.");

        revia::process::ProcessSettings process;
        process.enabled = true;
        process.allowCommandInterpreters = true;
        process.allowTaskExecution = true;
        Require(editor.SetProcessSettings(path, process, error) && revia::policy::PermissionStore().Load(path, saved, error) &&
                saved.process.allowCommandInterpreters && saved.process.allowTaskExecution,
            "Shells and delegated programs could not be allowed.");
        process.enabled = false;
        Require(editor.SetProcessSettings(path, process, error) && revia::policy::PermissionStore().Load(path, saved, error) &&
                !saved.process.enabled && !saved.process.allowCommandInterpreters && !saved.process.allowTaskExecution,
            "Shells or delegated programs survived running programs being turned off.");

        // The visible browser asks before it is turned on; internet access coming back must not
        // bring it back without that question.
        Require(editor.SetInternetAccess(path, true, true, error) && editor.SetInternetBrowser(path, true, true, error) &&
                    revia::policy::PermissionStore().Load(path, saved, error) && saved.internet.visibleBrowser &&
                    saved.internet.autonomousResearch,
            "The visible browser could not be allowed.");
        Require(editor.SetInternetAccess(path, false, true, error) && revia::policy::PermissionStore().Load(path, saved, error) &&
                    !saved.internet.enabled && !saved.internet.visibleBrowser && !saved.internet.autonomousResearch,
            "The visible browser survived internet access being turned off.");
        Require(editor.SetInternetAccess(path, true, true, error) && revia::policy::PermissionStore().Load(path, saved, error) &&
                    saved.internet.enabled && !saved.internet.visibleBrowser,
            "Turning internet access on again brought the visible browser back unasked.");
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
    file.VerifyInnerGrantsAreWithdrawn();
}
