#include "Agents/checkExecutor.h"

#include "Actions/actionTypes.h"
#include "Core/stdioProcess.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

namespace revia::agents
{

namespace
{
constexpr std::size_t MaximumNamedPaths = 4;
constexpr std::size_t MaximumDirectoryEntries = 200;
constexpr std::uintmax_t LongestFileRead = 512 * 1024;

std::string Lower(const std::string& text)
{
    std::string lowered;
    lowered.reserve(text.size());
    for (const unsigned char character : text)
    {
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    return lowered;
}

bool WholeWord(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return false;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
        at = haystack.find(needle, at + 1))
    {
        const bool startsClean = at == 0 || std::isalnum(static_cast<unsigned char>(haystack[at - 1])) == 0;
        const std::size_t end = at + needle.size();
        const bool endsClean = end >= haystack.size() ||
            std::isalnum(static_cast<unsigned char>(haystack[end])) == 0;
        if (startsClean && endsClean) return true;
    }
    return false;
}

bool Under(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt)
    {
        if (rootIt->empty()) continue;
        if (candidateIt == candidate.end() || *candidateIt != *rootIt) return false;
    }
    return true;
}

std::string Tail(const std::string& text, const std::size_t maximum, bool& outTruncated)
{
    outTruncated = text.size() > maximum;
    if (!outTruncated) return text;
    std::string tail = text.substr(text.size() - maximum);
    // Start at a line, so the first line shown is a whole one.
    const std::size_t newline = tail.find('\n');
    if (newline != std::string::npos && newline + 1 < tail.size()) tail.erase(0, newline + 1);
    return tail;
}
} // namespace

ConfinedCheckExecutor::ConfinedCheckExecutor(ConfinedCheckSettings inputSettings, Reporter inputReport)
    : settings(std::move(inputSettings)), report(std::move(inputReport))
{
    if (!settings.workspace.empty())
    {
        std::error_code error;
        settings.workspace = std::filesystem::absolute(settings.workspace, error).lexically_normal();
    }
    if (!settings.logDirectory.empty())
    {
        std::error_code error;
        settings.logDirectory = std::filesystem::absolute(settings.logDirectory, error).lexically_normal();
    }
}

bool ConfinedCheckExecutor::Available() const
{
    std::error_code error;
    return !settings.commands.empty() ||
        (!settings.workspace.empty() && std::filesystem::is_directory(settings.workspace, error));
}

std::string ConfinedCheckExecutor::Describe() const
{
    if (!Available()) return {};
    std::ostringstream text;
    std::error_code error;
    const bool workspaceExists = !settings.workspace.empty() &&
        std::filesystem::is_directory(settings.workspace, error);
    if (workspaceExists)
    {
        text << "Files under " << actions::PathToUtf8(settings.workspace)
             << " can be read (source, config, file state): name the path in <what you did>. ";
        if (!settings.logDirectory.empty())
        {
            text << "Her own logs under " << actions::PathToUtf8(settings.logDirectory)
                 << " can be read the same way (logs). ";
        }
    }
    if (!settings.commands.empty())
    {
        text << "These checks can be run by name (tests): ";
        for (std::size_t index = 0; index < settings.commands.size(); ++index)
        {
            const CheckCommand& command = settings.commands[index];
            text << (index == 0 ? "" : ", ") << command.name << " (" << command.command;
            for (const std::string& argument : command.arguments) text << " " << argument;
            text << ")";
        }
        text << ". ";
    }
    text << "Nothing else can be consulted; a check that names none of these is reasoning.";
    return text.str();
}

std::optional<CheckCommand> ConfinedCheckExecutor::MatchCommand(
    const std::vector<CheckCommand>& commands,
    const std::string& description,
    const std::string& questionText)
{
    const std::string lowered = Lower(description + " " + questionText);
    for (const CheckCommand& command : commands)
    {
        if (WholeWord(lowered, Lower(command.name))) return command;
    }
    if (commands.size() == 1) return commands.front();
    return std::nullopt;
}

std::vector<std::filesystem::path> ConfinedCheckExecutor::NamedPaths(
    const std::string& text,
    const std::vector<std::filesystem::path>& roots)
{
    std::vector<std::filesystem::path> found;
    if (roots.empty()) return found;
    std::string token;
    const auto consider = [&](std::string candidate)
    {
        while (!candidate.empty() && std::string(".,;:!?)\"'`").find(candidate.back()) != std::string::npos)
        {
            candidate.pop_back();
        }
        while (!candidate.empty() && std::string("(\"'`").find(candidate.front()) != std::string::npos)
        {
            candidate.erase(0, 1);
        }
        if (candidate.size() < 2 || found.size() >= MaximumNamedPaths) return;
        // A bare word counts only when something of that name exists under a root, so
        // "list src" lists the src folder and "the" names nothing.
        if (std::all_of(candidate.begin(), candidate.end(),
                [](const unsigned char character) { return std::isdigit(character) != 0; }))
        {
            return;
        }
        const std::filesystem::path named = actions::Utf8ToPath(candidate);
        std::error_code error;
        for (const std::filesystem::path& root : roots)
        {
            const std::filesystem::path absoluteRoot = std::filesystem::absolute(root, error).lexically_normal();
            const std::filesystem::path resolved =
                (named.is_absolute() ? named : absoluteRoot / named).lexically_normal();
            if (!Under(absoluteRoot, resolved)) continue;
            if (!std::filesystem::exists(resolved, error)) continue;
            // Canonically inside as well, so a link cannot lead out.
            const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved, error);
            if (error || !Under(canonicalRoot, canonical)) continue;
            if (std::find(found.begin(), found.end(), resolved) == found.end()) found.push_back(resolved);
            return;
        }
    };
    for (const char character : text)
    {
        if (std::isspace(static_cast<unsigned char>(character)) != 0 || character == '|')
        {
            consider(token);
            token.clear();
        }
        else
        {
            token.push_back(character);
        }
    }
    consider(token);
    return found;
}

ExecutedCheck ConfinedCheckExecutor::Execute(
    const CheckKind kind,
    const std::string& description,
    const std::string& questionText,
    const std::stop_token stopToken) const
{
    ExecutedCheck executed;
    switch (kind)
    {
        case CheckKind::Tests:
        {
            const std::optional<CheckCommand> command =
                MatchCommand(settings.commands, description, questionText);
            if (!command)
            {
                executed.refusal = settings.commands.empty()
                    ? "no check commands are configured; nothing can be run"
                    : "the check names none of the configured commands";
                return executed;
            }
            return RunCommand(*command, stopToken);
        }
        case CheckKind::SourceCode:
        case CheckKind::ResolvedConfiguration:
        case CheckKind::FileOrApplicationState:
            return ReadNamed(description, questionText, {settings.workspace}, "the workspace");
        case CheckKind::LogsAndMeasurements:
        {
            std::vector<std::filesystem::path> roots;
            if (!settings.logDirectory.empty()) roots.push_back(settings.logDirectory);
            if (!settings.workspace.empty()) roots.push_back(settings.workspace);
            return ReadNamed(description, questionText, roots, "her logs or the workspace");
        }
        case CheckKind::Research:
            executed.refusal = "research is not a check here; it goes through her lookup path";
            return executed;
        case CheckKind::Calculation:
            executed.refusal = "no calculator is wired; a calculation stays reasoning";
            return executed;
        case CheckKind::ModelReasoning:
            executed.refusal = "reasoning needs no executor";
            return executed;
    }
    executed.refusal = "an unknown kind of check";
    return executed;
}

ExecutedCheck ConfinedCheckExecutor::RunCommand(const CheckCommand& command, const std::stop_token stopToken) const
{
    ExecutedCheck executed;
    std::error_code error;
    if (settings.workspace.empty() || !std::filesystem::is_directory(settings.workspace, error))
    {
        executed.refusal = "the workspace does not exist, so nothing can run in it";
        return executed;
    }
    if (report) report("Running", command.name + ": " + command.command);
    core::StdioLaunch launch;
    launch.command = command.command;
    launch.arguments = command.arguments;
    launch.workingDirectory = settings.workspace;
    launch.logName = "check-" + command.name;
    launch.displayName = "check '" + command.name + "'";
    launch.memoryLimitMiB = settings.memoryLimitMiB;
    launch.cpuSecondsLimit = std::max(1, command.timeoutSeconds);
    core::StdioProcess process;
    std::string startError;
    if (!process.Start(launch, startError))
    {
        executed.refusal = "the check could not start: " + startError;
        return executed;
    }
    // Nothing to say to it: a check that waits for input would wait for ever.
    process.CloseInput();
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(std::max(1, command.timeoutSeconds));
    std::string output;
    bool closed = false;
    bool stopped = false;
    bool timedOut = false;
    while (!closed)
    {
        std::string line;
        if (process.ReadLine(line, std::chrono::milliseconds(200), closed))
        {
            output += line;
            output += '\n';
            // Bounded while it runs, not only at the end: a check that prints without
            // end must not grow without end in memory.
            if (output.size() > settings.maximumOutputCharacters * 4)
            {
                output.erase(0, output.size() - settings.maximumOutputCharacters * 2);
            }
            continue;
        }
        if (closed) break;
        if (stopToken.stop_requested())
        {
            stopped = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            timedOut = true;
            break;
        }
    }
    if (!stopped && !timedOut)
    {
        // Output closed; the process may take a moment more to exit.
        for (int slice = 0; slice < 100 && process.IsRunning(); ++slice)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    process.Stop();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (stopped)
    {
        executed.refusal = "the check was stopped";
        if (report) report("Stopped", command.name);
        return executed;
    }
    bool truncated = false;
    const std::string shown = Tail(output, settings.maximumOutputCharacters, truncated);
    std::ostringstream observed;
    observed.precision(1);
    observed.setf(std::ios::fixed);
    if (timedOut)
    {
        observed << "'" << command.name << "' did not finish within " << command.timeoutSeconds
                 << " s and was stopped.";
    }
    else
    {
        observed << "'" << command.name << "' exited with code " << process.ExitCode()
                 << " after " << seconds << " s.";
    }
    if (shown.empty()) observed << " It printed nothing.";
    else observed << "\nOutput" << (truncated ? " (last part)" : "") << ":\n" << shown;
    executed.ran = true;
    executed.observed = observed.str();
    if (truncated)
    {
        executed.limitations = "Only the last " + std::to_string(settings.maximumOutputCharacters) +
            " characters of the output were kept.";
    }
    if (timedOut)
    {
        executed.limitations += (executed.limitations.empty() ? "" : " ") +
            std::string("The check did not complete, so its result is not known.");
    }
    if (report)
    {
        report(timedOut ? "Timed out" : "Ran",
            command.name + (timedOut ? "" : " (exit " + std::to_string(process.ExitCode()) + ")"));
    }
    return executed;
}

ExecutedCheck ConfinedCheckExecutor::ReadNamed(
    const std::string& description,
    const std::string& questionText,
    const std::vector<std::filesystem::path>& roots,
    const char* what) const
{
    ExecutedCheck executed;
    std::vector<std::filesystem::path> usable;
    for (const std::filesystem::path& root : roots)
    {
        if (!root.empty()) usable.push_back(root);
    }
    const std::vector<std::filesystem::path> named = NamedPaths(description + " " + questionText, usable);
    if (named.empty())
    {
        executed.refusal = std::string("no existing file or folder under ") + what +
            " is named in the check";
        return executed;
    }
    std::ostringstream observed;
    std::size_t budget = settings.maximumOutputCharacters;
    bool truncated = false;
    std::error_code error;
    for (const std::filesystem::path& path : named)
    {
        if (report) report("Reading", actions::PathToUtf8(path));
        if (std::filesystem::is_directory(path, error))
        {
            observed << actions::PathToUtf8(path) << " (folder):\n";
            std::size_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(path, error))
            {
                if (++count > MaximumDirectoryEntries)
                {
                    observed << "  ...\n";
                    truncated = true;
                    break;
                }
                observed << "  " << actions::PathToUtf8(entry.path().filename())
                         << (entry.is_directory(error) ? "/" : "") << "\n";
            }
            continue;
        }
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
        {
            observed << actions::PathToUtf8(path) << ": could not be read.\n";
            continue;
        }
        std::ifstream stream(path, std::ios::binary);
        std::string content;
        if (size > LongestFileRead)
        {
            content.resize(static_cast<std::size_t>(LongestFileRead));
            stream.read(content.data(), static_cast<std::streamsize>(content.size()));
            truncated = true;
        }
        else
        {
            content.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        }
        if (content.size() > budget)
        {
            content = utf8::Prefix(content, budget);
            truncated = true;
        }
        budget -= std::min(budget, content.size());
        observed << actions::PathToUtf8(path) << " (" << size << " bytes):\n" << content;
        if (content.empty() || content.back() != '\n') observed << "\n";
        if (budget == 0) break;
    }
    executed.ran = true;
    executed.observed = observed.str();
    if (truncated)
    {
        executed.limitations = "Not all of it was shown; the content was cut to " +
            std::to_string(settings.maximumOutputCharacters) + " characters.";
    }
    return executed;
}

} // namespace revia::agents
