#pragma once

#include <filesystem>
#include <string>

namespace revia::core
{

// Makes a dying process say why.
//
// A WIN32 GUI application has no console, so an uncaught exception, a std::terminate, a
// Qt fatal, or a stack overflow all end the same way: the window vanishes and nothing is
// written anywhere. That is indistinguishable from the user closing it, which makes the
// difference between "it crashed" and "I closed it" unanswerable -- and a bug that cannot
// be told from normal behaviour cannot be fixed.
//
// Everything here is best-effort by nature: the process is already failing. It is still
// worth far more than silence.
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
