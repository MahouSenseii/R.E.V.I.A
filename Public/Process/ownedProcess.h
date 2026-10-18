#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace revia::process
{

struct OwnedProcessRequest
{
    // Absolute path. Started directly, never through a shell, so no argument is ever
    // parsed as a command.
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    // The complete environment beyond the platform minimum. The parent's is not inherited.
    std::map<std::string, std::string> environment;
    // Empty means the executable's own folder.
    std::filesystem::path workingDirectory;
    // Standard output the parent has not read yet. Past it the child is stopped: a
    // reader that cannot keep up must not let a child grow Revia's memory without bound.
    std::size_t maxPendingOutputBytes = 8U * 1024U * 1024U;
    // Only the newest standard error is kept, for diagnostics.
    std::size_t maxErrorBytes = 16384;
};

// A long-lived child with pipes on all three standard streams, owned like the one-shot
// process executor's children: on Windows a kill-on-close job object holds the whole
// process tree, the handle list is explicit and the environment starts empty; elsewhere
// the child leads its own process group and the group is killed. Destroying the owner
// stops the tree.
class OwnedProcess
{
  public:
    OwnedProcess();
    ~OwnedProcess();
    OwnedProcess(const OwnedProcess&) = delete;
    OwnedProcess& operator=(const OwnedProcess&) = delete;

    [[nodiscard]] bool Start(const OwnedProcessRequest& request, std::string& error);
    // Writes all of `bytes` to the child's standard input.
    [[nodiscard]] bool Write(const std::string& bytes, std::string& error);
    // Whatever standard output arrives within `wait`, possibly nothing.
    [[nodiscard]] std::string Read(std::chrono::milliseconds wait);
    // False once the child has exited or been stopped and all its output was read.
    [[nodiscard]] bool Running() const;
    // The newest standard error, bounded, for a status line.
    [[nodiscard]] std::string ErrorOutput() const;
    // The same once the child has gone, after its standard error has been read to the end
    // or `wait` has passed. Standard error is read apart from standard output, so the end
    // of one says nothing about the other: a child that has just exited may still have
    // its last words in the pipe.
    [[nodiscard]] std::string FinalErrorOutput(std::chrono::milliseconds wait) const;
    // A reason when the owner stopped the child itself, such as unread output overflowing.
    [[nodiscard]] std::string StopReason() const;
    void Terminate();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace revia::process
