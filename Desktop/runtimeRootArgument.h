#pragma once

#include <filesystem>
#include <string>

namespace revia::desktop
{
// Startup-only host configuration; it never searches or migrates private stores.
class RuntimeRootArgument
{
  public:
    bool Accept(const std::string& value)
    {
        if (specified) return false;
        specified = true;
        try
        {
            const auto candidate = std::filesystem::path(std::u8string(value.begin(), value.end())).lexically_normal();
            std::error_code error;
            if (candidate.empty() || !candidate.is_absolute() ||
                !std::filesystem::is_directory(candidate, error) || error ||
                !std::filesystem::is_directory(candidate / "Config", error) || error)
                return false;
            selected = candidate;
            return true;
        }
        catch (...) { return false; }
    }
    [[nodiscard]] std::filesystem::path Directory(const std::filesystem::path& executableDirectory) const
    { return specified ? selected : executableDirectory; }
    [[nodiscard]] bool Specified() const { return specified; }
    [[nodiscard]] const char* Error() const
    { return "--runtime-root requires one absolute existing directory containing Config."; }
  private:
    bool specified = false;
    std::filesystem::path selected;
};
}
