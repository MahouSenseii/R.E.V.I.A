#include "Workspace/virtualMachineSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::workspace
{
namespace
{
VirtualMachineSettingsResult Invalid()
{
    return {false, {}, "invalid_settings", "Virtual machine startup settings need owner review."};
}

bool ValidOwner(const std::string& owner)
{
    return !owner.empty() && owner.size() <= 64 && std::all_of(owner.begin(), owner.end(), [](const unsigned char value)
    { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
             value == '_' || value == '-'; });
}

bool IsRedirected(const std::filesystem::path& path)
{
    std::filesystem::path cursor;
    for (const auto& component : path)
    {
        cursor /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(cursor, error);
        if (std::filesystem::is_symlink(status))
            return true;
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
            return true;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return true;
#endif
    }
    return false;
}
}

VirtualMachineSettingsResult LoadVirtualMachineSettings(
    const std::filesystem::path& path, const std::string& expectedOwnerId)
{
    try
    {
        if (!ValidOwner(expectedOwnerId) || path.empty() || IsRedirected(path))
            return Invalid();
        std::error_code error;
        const auto status = std::filesystem::status(path, error);
        if (error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(status)))
            return {true, {}, "disabled", "Virtual machine startup is disabled."};
        if (error || !std::filesystem::is_regular_file(status))
            return Invalid();

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return {false, {}, "settings_unavailable", "Virtual machine startup settings could not be read."};
        std::array<char, 4097> bytes{};
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = stream.gcount();
        if (stream.bad() || count <= 0 || count > 4096)
            return Invalid();

        bool duplicate = false;
        std::set<std::string> keys;
        const auto data = nlohmann::json::parse(bytes.begin(), bytes.begin() + count,
            [&](const int, const nlohmann::json::parse_event_t event, nlohmann::json& parsed)
            {
                if (event == nlohmann::json::parse_event_t::key && !keys.insert(parsed.get<std::string>()).second)
                    duplicate = true;
                return true;
            });
        if (duplicate || !data.is_object() || data.size() != 3 || !data.contains("schemaVersion") ||
            !data.contains("enabled") || !data.contains("ownerId") || !data["schemaVersion"].is_number_integer() ||
            data["schemaVersion"] != 1 || !data["enabled"].is_boolean() || !data["ownerId"].is_string() ||
            data["ownerId"].get<std::string>() != expectedOwnerId)
            return Invalid();

        const bool enabled = data["enabled"].get<bool>();
        return {true, {enabled, expectedOwnerId}, enabled ? "enabled" : "disabled",
            enabled ? "Virtual machine startup is enabled for this companion." : "Virtual machine startup is disabled."};
    }
    catch (...)
    {
        return Invalid();
    }
}
}
