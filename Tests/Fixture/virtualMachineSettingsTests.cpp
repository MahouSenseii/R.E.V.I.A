#include "Workspace/virtualMachineSettings.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Check(const bool condition, const std::string& reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

struct TemporaryDirectory
{
    std::filesystem::path root;
    TemporaryDirectory()
    {
        const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt != 100; ++attempt)
        {
            const auto candidate = std::filesystem::temp_directory_path() /
                ("revia-vm-settings-tests-" + std::to_string(clock) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate))
            {
                root = candidate;
                return;
            }
        }
        throw std::runtime_error("The settings fixture directory could not be created.");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        if (root.parent_path() == std::filesystem::temp_directory_path() &&
            root.filename().string().starts_with("revia-vm-settings-tests-"))
            std::filesystem::remove_all(root, error);
    }
};

void Write(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.close();
    Check(!stream.fail(), "The fixture could not write settings.");
}

void TestExplicitCompanionOptIn()
{
    TemporaryDirectory directory;
    const auto file = directory.root / "settings.json";
    Write(file, R"({"schemaVersion":1,"enabled":true,"ownerId":"companion-a"})");
    const auto enabled = revia::workspace::LoadVirtualMachineSettings(file, "companion-a");
    Check(enabled.succeeded && enabled.settings.enabled && enabled.settings.ownerId == "companion-a",
        "An explicit matching companion startup opt-in was not admitted.");
    const auto another = revia::workspace::LoadVirtualMachineSettings(file, "companion-b");
    Check(!another.succeeded && !another.settings.enabled && another.settings.ownerId.empty(),
        "A companion borrowed another companion's virtual machine opt-in.");
    Write(file, R"({"schemaVersion":1,"enabled":false,"ownerId":"companion-a"})");
    const auto disabled = revia::workspace::LoadVirtualMachineSettings(file, "companion-a");
    Check(disabled.succeeded && !disabled.settings.enabled, "An explicit disabled setting enabled startup.");
}

void TestMissingSettingsStayDisabledWithoutWrites()
{
    TemporaryDirectory directory;
    const auto missing = directory.root / "not-created" / "settings.json";
    const auto result = revia::workspace::LoadVirtualMachineSettings(missing, "companion-a");
    Check(result.succeeded && !result.settings.enabled && result.settings.ownerId.empty(),
        "Absent companion settings did not default to disabled.");
    Check(!std::filesystem::exists(missing.parent_path()), "Reading missing settings created runtime state.");
}

void TestMalformedAuthorityFailsClosed()
{
    TemporaryDirectory directory;
    const auto file = directory.root / "settings.json";
    const std::vector<std::string> invalid = {
        "not json private-secret-sentinel",
        R"([])",
        R"({"schemaVersion":"1","enabled":true,"ownerId":"companion-a"})",
        R"({"schemaVersion":2,"enabled":true,"ownerId":"companion-a"})",
        R"({"schemaVersion":1,"enabled":1,"ownerId":"companion-a"})",
        R"({"schemaVersion":1,"ownerId":"companion-a"})",
        R"({"schemaVersion":1,"enabled":true,"ownerId":null})",
        R"({"schemaVersion":1,"enabled":true,"ownerId":"companion-a","root":"private-secret-sentinel"})",
        R"({"schemaVersion":1,"enabled":false,"enabled":true,"ownerId":"companion-a"})",
        R"({"schemaVersion":1,"enabled":true,"ownerId":"companion-a"} trailing)",
        std::string(8192, ' ')
    };
    for (const auto& contents : invalid)
    {
        Write(file, contents);
        const auto result = revia::workspace::LoadVirtualMachineSettings(file, "companion-a");
        Check(!result.succeeded && !result.settings.enabled && result.settings.ownerId.empty() && !result.code.empty(),
            "Malformed virtual machine authority did not fail closed.");
        Check(result.message.find("private-secret-sentinel") == std::string::npos &&
                  result.message.find(file.string()) == std::string::npos,
            "A settings failure exposed private file contents or paths in status.");
    }
    const auto directoryResult = revia::workspace::LoadVirtualMachineSettings(directory.root, "companion-a");
    Check(!directoryResult.succeeded && !directoryResult.settings.enabled, "A directory was accepted as authority settings.");
    Write(file, R"({"schemaVersion":1,"enabled":true,"ownerId":""})");
    const auto unknownOwner = revia::workspace::LoadVirtualMachineSettings(file, "");
    Check(!unknownOwner.succeeded && !unknownOwner.settings.enabled, "An empty companion identity enabled startup.");
}
}

int main()
{
    try
    {
        TestExplicitCompanionOptIn();
        TestMissingSettingsStayDisabledWithoutWrites();
        TestMalformedAuthorityFailsClosed();
        std::cout << "Virtual machine startup opt-in stays explicit, companion-local and fail-closed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
