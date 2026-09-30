#pragma once

#include <filesystem>
#include <string>

namespace revia::core
{

// Best-effort diagnostics for uncaught exceptions, termination and fatal process faults.
class CrashDiagnostics
{
public:
    // Installs the handlers and records where to write. Safe to call once, early, before
    // any window exists.
    static void Install(std::filesystem::path logDirectory);

    // Appends one line to the crash log. Used by the handlers, and available for the Qt
    // message hook so a qFatal lands somewhere readable.
    static void Record(const std::string& category, const std::string& message);

    [[nodiscard]] static std::filesystem::path LogPath();

private:
    static void InstallPlatformHandlers();
};

} // namespace revia::core
