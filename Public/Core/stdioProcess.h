#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::core
{

// A child process Revia talks to over its standard input and output, line by line.
//
// The Python workers she starts speak HTTP on loopback and never need their pipes; an
// agent speaking the Agent Client Protocol speaks JSON-RPC over stdio and needs nothing
// else. Same ownership as the workers -- her log directory for its stderr, a job
// object on Windows so it dies with her -- and the pipes on top. POSIX has the same
// shape so the protocol can be tested where the tests run.
struct StdioLaunch
{
    // Resolved against the runtime root when that names a file; otherwise found on PATH.
    std::string command;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    // stderr goes to <logName>.stderr.log in her log directory.
    std::string logName = "child";
    // What the errors call it.
    std::string displayName = "child process";
    // Limits on the child and everything it starts; 0 leaves one unset. Memory is the
    // process's commit on Windows and its address space elsewhere; CPU seconds are
    // the job's user time on Windows and the process's on POSIX.
    std::uint64_t memoryLimitMiB = 0;
    int cpuSecondsLimit = 0;
};

class StdioProcess
{
public:
    StdioProcess() = default;
    ~StdioProcess();

    StdioProcess(const StdioProcess&) = delete;
    StdioProcess& operator=(const StdioProcess&) = delete;

    bool Start(const StdioLaunch& launch, std::string& outError);
    [[nodiscard]] bool IsRunning() const;
    // Everything given, to the child's stdin. False once the pipe is gone.
    bool Write(const std::string& bytes);
    // One line without its newline. False on a timeout (outLine untouched) and when the
    // child closed its stdout, which outClosed says.
    bool ReadLine(std::string& outLine, std::chrono::milliseconds timeout, bool& outClosed);
    // Closes the child's stdin, so a child that reads until end of input can finish.
    void CloseInput();
    // The exit code once the child has ended; -1 while it runs or when unknown.
    [[nodiscard]] int ExitCode() const;
    // Ends the child if it is still running, and reaps it.
    void Stop();

private:
    bool Fill(std::chrono::milliseconds timeout, bool& outClosed);

    std::string pending;
    int exitCode = -1;
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
    void* stdinWrite = nullptr;
    void* stdoutRead = nullptr;
#else
    int pid = -1;
    int stdinWrite = -1;
    int stdoutRead = -1;
#endif
};

} // namespace revia::core
