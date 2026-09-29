#pragma once

#include <string>
#include <vector>

namespace revia::core
{

// One Python worker Revia starts and owns: a script in its own environment, with the
// arguments she chooses, its output in her log directory, killed with her by a job
// object. The Qwen voice worker grew this shape first; this is the same shape for the
// workers that came after it. Windows only, like every process she launches: on
// another platform Start says so and the caller falls back or refuses.
struct PythonWorkerLaunch
{
    // Resolved against the runtime root; the script's directory is the working one.
    std::string script;
    std::string pythonExecutable = "python";
    std::vector<std::string> arguments;
    // Log files are <logName>-<port>.stdout.log and .stderr.log.
    std::string logName = "worker";
    int port = 0;
    // What the errors call it.
    std::string displayName = "Python worker";
};

class PythonWorkerProcess
{
public:
    PythonWorkerProcess() = default;
    ~PythonWorkerProcess();

    PythonWorkerProcess(const PythonWorkerProcess&) = delete;
    PythonWorkerProcess& operator=(const PythonWorkerProcess&) = delete;

    bool Start(const PythonWorkerLaunch& launch, std::string& outError);
    [[nodiscard]] bool IsRunning() const;
    void Stop();

private:
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
#endif
};

} // namespace revia::core
