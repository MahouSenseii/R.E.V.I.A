#pragma once

#include "Agents/investigationAgent.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

// A check the investigation loop can actually run.
//
// The loop's executor seam was left empty on purpose: without one, every "I ran the
// tests" a model writes is recorded as reasoning, because nothing ran. This fills the
// seam with the two kinds of check that can be made honest and bounded. A configured
// command -- the owner names it, its program and its arguments in settings, the way
// an MCP manifest pins a tool -- is run in the workspace with no input, a time limit,
// a memory limit and a job to kill the whole tree, and its exit code and the tail of
// its output become the observation. A file named in the check, under the workspace
// (or, for logs, under her log directory), is read and its content becomes the
// observation. Research and calculation are not checks here and are refused as such.
//
// "Confined" is the honest word: the command list is the trust boundary, not a
// sandbox. A command the owner listed runs with the owner's rights, in a job that
// limits how long and how much it may take.
struct CheckCommand
{
    // What a check names it by: "tests", "build".
    std::string name;
    std::string command;
    std::vector<std::string> arguments;
    int timeoutSeconds = 300;
};

struct ConfinedCheckSettings
{
    // Absolute. Commands run here; files are read from under here.
    std::filesystem::path workspace;
    std::vector<CheckCommand> commands;
    // Read-only, for LogsAndMeasurements. Empty means logs are not offered.
    std::filesystem::path logDirectory;
    std::size_t maximumOutputCharacters = 6000;
    std::uint64_t memoryLimitMiB = 2048;
};

class ConfinedCheckExecutor
{
public:
    // `report` sees each check as it runs: the phase and a line about it.
    using Reporter = std::function<void(const std::string& phase, const std::string& message)>;

    explicit ConfinedCheckExecutor(ConfinedCheckSettings settings, Reporter report = {});

    // Whether anything can be checked at all: a workspace that exists, or a command.
    [[nodiscard]] bool Available() const;
    // What to tell the model it may ask for. Empty when nothing is available.
    [[nodiscard]] std::string Describe() const;

    [[nodiscard]] ExecutedCheck Execute(
        CheckKind kind,
        const std::string& description,
        const std::string& questionText,
        std::stop_token stopToken = {}) const;

    // The pieces on their own, for the tests.
    // The configured command a check names, by its name as a whole word; the only
    // command when there is one and the check names none.
    [[nodiscard]] static std::optional<CheckCommand> MatchCommand(
        const std::vector<CheckCommand>& commands,
        const std::string& description,
        const std::string& questionText);
    // Paths named in the text that exist under one of the roots. Relative ones are
    // taken against the first root. At most four, in the order named.
    [[nodiscard]] static std::vector<std::filesystem::path> NamedPaths(
        const std::string& text,
        const std::vector<std::filesystem::path>& roots);

private:
    [[nodiscard]] ExecutedCheck RunCommand(const CheckCommand& command, std::stop_token stopToken) const;
    [[nodiscard]] ExecutedCheck ReadNamed(
        const std::string& description,
        const std::string& questionText,
        const std::vector<std::filesystem::path>& roots,
        const char* what) const;

    ConfinedCheckSettings settings;
    Reporter report;
};

} // namespace revia::agents
