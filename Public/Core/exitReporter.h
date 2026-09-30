#pragma once

#include <filesystem>
#include <string>

namespace revia::core
{

enum class ExitReason
{
    // The user typed /exit, /quit, or /bye.
    UserCommand,
    // Quit from the tray menu.
    TrayQuit,
    // The window was closed and no tray icon was there to keep the process alive.
    WindowClosed,
    // A smoke-test switch asked for shutdown.
    SmokeTest,
    // The runtime could not start, so there was nothing to keep running.
    StartupFailure,
    // A fault, an uncaught exception, or a fatal signal. The detail names which.
    Crash,
    // The Qt event loop returned without anything more specific having been recorded.
    EventLoopEnded,
    // Windows is logging out or shutting down and asked the application to close.
    SystemShutdown,
    // Inferred on the *next* start, because a process killed with TerminateProcess, a
    // power loss, or a fault too severe to run a handler cannot record anything itself.
    ExternalTermination
};

[[nodiscard]] std::string ToString(ExitReason reason);

// Writes a session marker at startup and removes it only after recording an exit reason.
// A surviving marker identifies the prior unexplained exit on the next startup.
class ExitReporter
{
public:
    // Opens a session. Returns a description of the previous session's unrecorded exit
    // when there was one, so the caller can log it through its own logger as well.
    static std::string Begin(std::filesystem::path logDirectory);

    // The first reason wins. A crash records itself, and the event-loop exit that follows
    // must not overwrite it with something generic and true-but-useless.
    static void Record(ExitReason reason, const std::string& detail = {});

    [[nodiscard]] static bool HasRecorded();
    // The previous session's unrecorded exit, if there was one, so a caller that has a
    // logger can put it in the log a human actually reads.
    [[nodiscard]] static std::string PreviousUncleanExit();
    [[nodiscard]] static std::filesystem::path LedgerPath();

private:
    [[nodiscard]] static std::filesystem::path MarkerPath();
};

} // namespace revia::core
