#include "Agents/sandboxTools.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"
#include "Workspace/guestReceipt.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

namespace revia::agents
{
namespace
{
using Json = nlohmann::json;

constexpr std::size_t MaximumCodeBytes = 30000;
constexpr std::size_t MaximumPatchArgumentBytes = 32000;
constexpr std::size_t ShownOutputBytes = 2400;
constexpr std::size_t ShownErrorBytes = 1200;
constexpr std::size_t MaximumReadLines = 400;
constexpr std::size_t MaximumPatchFiles = 20;
constexpr int SearchTimeoutMs = 20000;
constexpr int TestTimeoutMs = 120000;

Json StringSchema(const std::size_t minimum, const std::size_t maximum, const std::string& description)
{
    return {{"type", "string"}, {"minLength", minimum}, {"maxLength", maximum}, {"description", description}};
}

std::string ObjectSchema(Json properties, const std::vector<std::string>& required)
{
    return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", required}, {"additionalProperties", false}}.dump();
}

std::string Text(const Json& arguments, const char* key)
{
    return arguments.contains(key) && arguments[key].is_string() ? arguments[key].get<std::string>() : std::string{};
}

int Integer(const Json& arguments, const char* key, const int fallback)
{
    return arguments.contains(key) && arguments[key].is_number_integer() ? arguments[key].get<int>() : fallback;
}

bool Flag(const Json& arguments, const char* key)
{
    return arguments.contains(key) && arguments[key].is_boolean() && arguments[key].get<bool>();
}

std::string OneLine(const std::string& value, const std::size_t maximum = 80)
{
    std::string line = utf8::Sanitize(value);
    std::replace(line.begin(), line.end(), '\n', ' ');
    std::replace(line.begin(), line.end(), '\r', ' ');
    return line.size() > maximum ? utf8::Prefix(line, maximum) + "..." : line;
}

std::string ProjectList(const std::vector<std::string>& projects)
{
    if (projects.empty())
        return " No project is shared yet: share one with share_project first.";
    std::string listed;
    for (std::size_t index = 0; index < projects.size() && index < 8; ++index)
        listed += (index == 0 ? "" : ", ") + projects[index];
    return " Shared projects: " + listed + ".";
}

std::string RootList(const std::vector<std::filesystem::path>& roots)
{
    std::string listed;
    for (std::size_t index = 0; index < roots.size() && index < 6; ++index)
        listed += (index == 0 ? "" : "; ") + actions::PathToUtf8(roots[index]);
    return listed;
}

// The end of a text, where a traceback or a failing test names what went wrong.
std::string Tail(const std::string& text, const std::size_t maximum)
{
    if (text.size() <= maximum)
        return text;
    std::size_t start = text.size() - maximum;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
        ++start;
    return "[... earlier output omitted]\n" + text.substr(start);
}

std::string Head(const std::string& text, const std::size_t maximum)
{
    return text.size() <= maximum ? text : utf8::Prefix(text, maximum) + "\n[... later output omitted]";
}

// A process receipt as the model reads it: how it ended, then its output.
std::string DescribeRun(const workspace::GuestResult& receipt, const int timeoutMs)
{
    std::string text = "exit code: " + std::to_string(receipt.exitCode);
    if (receipt.timedOut)
        text += " (stopped at the " + std::to_string(timeoutMs / 1000) + " s limit)";
    else if (receipt.cancelled)
        text += " (cancelled)";
    else if (receipt.exitCode < 0)
        text += " (killed by signal " + std::to_string(-receipt.exitCode) + ", for example the CPU or memory limit)";
    if (receipt.outputTruncated)
        text += "\nThe output reached its limit and was cut.";
    text += "\nstdout:\n" + (receipt.standardOutput.empty() ? std::string("(empty)") : Head(receipt.standardOutput, ShownOutputBytes));
    if (!receipt.standardError.empty())
        text += "\nstderr:\n" + Tail(receipt.standardError, ShownErrorBytes);
    return text;
}

std::string GuestProjectPath(const std::string& project, const std::string& path = {})
{
    return "projects/" + project + (path.empty() ? std::string{} : "/" + path);
}

actions::ActionRequest GuestRequest(const actions::ActionType type, std::string path = {})
{
    actions::ActionRequest request;
    request.id = actions::NewActionId();
    request.type = type;
    request.guest.operation = *actions::GuestOperationForAction(type);
    request.guest.path = std::move(path);
    return request;
}

// A run name the worker accepts, ordered by time so older runs are pruned first.
std::string RunId()
{
    return "run-" + actions::NewActionId().substr(std::string("action-").size());
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool ArtifactNameAllowed(const std::string& name, std::string& extension)
{
    if (name.empty() || name.size() > 104 || name.front() == '.' || name.front() == '-')
        return false;
    for (const unsigned char c : name)
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-')
            return false;
    const auto dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    // Windows opens these as devices whatever their extension.
    const std::string stem = Lower(name.substr(0, name.find('.')));
    static const std::array<std::string, 6> devices = {"con", "prn", "aux", "nul", "com", "lpt"};
    for (const auto& device : devices)
        if (stem == device || (stem.size() == 4 && stem.compare(0, 3, device) == 0 && std::isdigit(static_cast<unsigned char>(stem[3]))))
            return false;
    extension = Lower(name.substr(dot));
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".gif" || extension == ".csv" ||
           extension == ".txt" || extension == ".json";
}

// Pictures are shown on the Canvas, so their bytes must be what their name says.
bool ImageMatches(const std::string& extension, const std::string& bytes)
{
    if (extension == ".png")
        return bytes.rfind("\x89PNG\r\n\x1a\n", 0) == 0;
    if (extension == ".jpg" || extension == ".jpeg")
        return bytes.size() > 3 && bytes.compare(0, 3, "\xFF\xD8\xFF") == 0;
    if (extension == ".gif")
        return bytes.rfind("GIF87a", 0) == 0 || bytes.rfind("GIF89a", 0) == 0;
    return utf8::IsValid(bytes) && bytes.find('\0') == std::string::npos;
}

void PruneOldest(const std::filesystem::path& folder, const std::string& prefix, const std::size_t keep)
{
    std::error_code error;
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator walk(folder, error), end; !error && walk != end; walk.increment(error))
    {
        const std::string name = actions::PathToUtf8(walk->path().filename());
        if (name.rfind(prefix, 0) == 0 && !walk->is_symlink())
            entries.push_back(walk->path());
    }
    std::sort(entries.begin(), entries.end());
    while (entries.size() > keep)
    {
        std::filesystem::remove_all(entries.front(), error);
        entries.erase(entries.begin());
    }
}

// ---- Lines and diffs -------------------------------------------------------------------

struct Lines
{
    std::vector<std::string> lines;
    bool finalNewline = true;
    bool crlf = false;
};

Lines Split(const std::string& text)
{
    Lines result;
    std::size_t carriageReturns = 0;
    std::size_t start = 0;
    while (start < text.size())
    {
        const auto end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
            ++carriageReturns;
        }
        result.lines.push_back(std::move(line));
        if (end == std::string::npos)
        {
            result.finalNewline = false;
            break;
        }
        start = end + 1;
    }
    result.crlf = carriageReturns * 2 > result.lines.size();
    return result;
}

std::string Join(const Lines& value)
{
    const std::string ending = value.crlf ? "\r\n" : "\n";
    std::string text;
    for (std::size_t index = 0; index < value.lines.size(); ++index)
    {
        text += value.lines[index];
        if (index + 1 < value.lines.size() || value.finalNewline)
            text += ending;
    }
    return text;
}

bool StartsWith(const std::string& text, const std::string& prefix)
{
    return text.rfind(prefix, 0) == 0;
}

std::string DiffPath(std::string value)
{
    const auto tab = value.find('\t');
    if (tab != std::string::npos)
        value.resize(tab);
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    if (value.size() > 1 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

bool ParseHunkStart(const std::string& line, std::size_t& oldStart)
{
    // "@@ -12,5 +12,6 @@ optional section"
    if (!StartsWith(line, "@@ -"))
        return false;
    std::size_t index = 4;
    std::size_t value = 0;
    bool digits = false;
    while (index < line.size() && std::isdigit(static_cast<unsigned char>(line[index])))
    {
        value = value * 10 + static_cast<std::size_t>(line[index] - '0');
        digits = true;
        if (value > 100000000)
            return false;
        ++index;
    }
    if (!digits || line.find(" +", index) == std::string::npos || line.find("@@", index) == std::string::npos)
        return false;
    oldStart = value;
    return true;
}

enum class Edit
{
    Same,
    Remove,
    Add
};

// An edit script: common ends trimmed, then the middle by longest common subsequence while
// it is small enough to afford, and as one replacement when it is not. Either is a correct
// diff; the second is only coarser.
std::vector<std::pair<Edit, std::string>> EditScript(const std::vector<std::string>& before, const std::vector<std::string>& after)
{
    std::size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix])
        ++suffix;
    const std::size_t rows = before.size() - prefix - suffix;
    const std::size_t columns = after.size() - prefix - suffix;
    std::vector<std::pair<Edit, std::string>> script;
    for (std::size_t index = 0; index < prefix; ++index)
        script.emplace_back(Edit::Same, before[index]);
    if (rows > 0 && columns > 0 && rows * columns <= 4000000)
    {
        std::vector<std::uint32_t> table((rows + 1) * (columns + 1), 0);
        const auto at = [&](const std::size_t row, const std::size_t column) -> std::uint32_t& { return table[row * (columns + 1) + column]; };
        for (std::size_t row = rows; row-- > 0;)
            for (std::size_t column = columns; column-- > 0;)
                at(row, column) = before[prefix + row] == after[prefix + column] ? at(row + 1, column + 1) + 1
                                                                                 : std::max(at(row + 1, column), at(row, column + 1));
        std::size_t row = 0;
        std::size_t column = 0;
        while (row < rows || column < columns)
        {
            if (row < rows && column < columns && before[prefix + row] == after[prefix + column])
            {
                script.emplace_back(Edit::Same, before[prefix + row]);
                ++row;
                ++column;
            }
            else if (column < columns && (row == rows || at(row, column + 1) >= at(row + 1, column)))
                script.emplace_back(Edit::Add, after[prefix + column++]);
            else
                script.emplace_back(Edit::Remove, before[prefix + row++]);
        }
    }
    else
    {
        for (std::size_t row = 0; row < rows; ++row)
            script.emplace_back(Edit::Remove, before[prefix + row]);
        for (std::size_t column = 0; column < columns; ++column)
            script.emplace_back(Edit::Add, after[prefix + column]);
    }
    for (std::size_t index = before.size() - suffix; index < before.size(); ++index)
        script.emplace_back(Edit::Same, before[index]);
    return script;
}

// The end-of-file marker travels inside a line for diffing, so a missing final newline
// counts as a change, and is written back out as git's "\ No newline" line.
const std::string NoNewline = std::string(1, '\0') + "no-newline";

// Lines as `git apply` compares them: a CRLF file keeps its carriage returns, or the diff
// would match nothing on disk.
std::vector<std::string> Tokens(const Lines& value)
{
    auto tokens = value.lines;
    for (std::size_t index = 0; value.crlf && index < tokens.size(); ++index)
        if (index + 1 < tokens.size() || value.finalNewline)
            tokens[index] += '\r';
    if (!value.finalNewline && !tokens.empty())
        tokens.back() += NoNewline;
    return tokens;
}

ChatTool Tool(std::string name, std::string description, std::string schema, const ToolEffect effect)
{
    ChatTool tool;
    tool.definition = {std::move(name), std::move(description), std::move(schema)};
    tool.effect = effect;
    // Each may ask the owner for approval, and prompts never overlap.
    tool.parallelSafe = false;
    return tool;
}
}

// ---- Projection -----------------------------------------------------------------------

SandboxToolPermissions ProjectSandboxTools(const policy::GuestCapabilitySettings& guest, const workspace::VirtualMachineSnapshot* guestState,
    const std::vector<std::filesystem::path>& approvedRoots, std::vector<std::string> projects)
{
    SandboxToolPermissions permissions;
    permissions.projects = std::move(projects);
    permissions.shareRoots = approvedRoots;
    if (!guest.enabled)
    {
        permissions.unavailableReason = "Off: guest actions are disabled in the capability settings (guest.enabled).";
        return permissions;
    }
    if (!guestState)
    {
        permissions.unavailableReason = "Off: the Ubuntu workspace is not enabled to start (RuntimeData/VirtualMachine/settings.json).";
        return permissions;
    }
    if (guestState->state != workspace::VirtualMachineState::Ready)
    {
        permissions.unavailableReason = std::string("Unavailable: the Ubuntu workspace is ") + workspace::ToString(guestState->state) +
                                        (guestState->code.empty() ? std::string{} : " (" + guestState->code + ")") + ".";
        return permissions;
    }
    const auto advertised = [guestState](const char* name)
    { return std::find(guestState->capabilities.begin(), guestState->capabilities.end(), name) != guestState->capabilities.end(); };
    const auto& approved = guest.limits.approvedExecutables;
    const bool python = std::find(approved.begin(), approved.end(), policy::GuestPython) != approved.end();
    permissions.projectRead = guest.read && advertised("search_text");
    permissions.share = guest.read && guest.write && advertised("import_files") && !approvedRoots.empty();
    permissions.projectWrite = guest.read && guest.write;
    permissions.projectRun = guest.execute;
    permissions.interpreter = guest.execute && python && advertised("run_python") && guestState->sandboxReady;
    if (!permissions.interpreter)
    {
        permissions.unavailableReason = !guest.execute ? "No interpreter: guest execution is off (guest.execute)."
                                        : !python      ? "No interpreter: /usr/bin/python3 is not an approved guest executable."
                                        : !advertised("run_python")
                                            ? "No interpreter: this guest was provisioned before the code tools; re-provision it to update its worker."
                                            : "No interpreter: the guest could not start an isolated process, so no run would be network-less.";
    }
    return permissions;
}

void AddSandboxTools(ToolRegistry& registry, const SandboxToolPermissions& permissions, const SandboxToolExecutors& executors)
{
    const std::string projects = ProjectList(permissions.projects);
    if (permissions.interpreter && executors.runPython)
    {
        auto tool = Tool("run_python",
            "Run Python 3 code in an isolated Ubuntu sandbox: no network, a fresh empty folder, CPU, memory and time limits. numpy, "
            "matplotlib and sympy are installed. Print what you need to stdout. To show files, save them under out/, for example "
            "plt.savefig('out/plot.png'); up to 4 png, jpg, gif, csv, txt or json files come back, and pictures appear on the Canvas. "
            "Use it for data work, plots and heavy math such as big numbers, algebra or statistics; for one simple expression use the "
            "calculator. Nothing persists between runs. The owner approves each run.",
            ObjectSchema({{"code", StringSchema(1, MaximumCodeBytes, "The complete Python program.")},
                             {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120},
                                                     {"description", "Wall-clock limit, 1 to 120 seconds. Default 30."}}}},
                {"code"}),
            ToolEffect::Effect);
        tool.maxArgumentBytes = MaximumCodeBytes + 1024;
        tool.describe = [](const Json& arguments)
        {
            const std::string code = Text(arguments, "code");
            const auto lines = static_cast<std::size_t>(std::count(code.begin(), code.end(), '\n')) + 1;
            return "Running " + std::to_string(lines) + (lines == 1 ? " line" : " lines") + " of Python in the sandbox";
        };
        tool.execute = [run = executors.runPython](const Json& arguments, const ToolCallContext& context)
        { return run(Text(arguments, "code"), Integer(arguments, "timeout_seconds", 30), context); };
        registry.Add(std::move(tool));
    }
    if (permissions.share && executors.shareProject)
    {
        auto tool = Tool("share_project",
            "Copy a folder of the owner's project into the Ubuntu sandbox so the coding tools can work on it, after the owner approves. "
            "Only text files are copied; version control, build output, binaries and credential files are left out. Nothing on this "
            "computer is mounted or changed, and sharing again replaces the earlier copy. Folders must be inside: " +
                RootList(permissions.shareRoots) + ".",
            ObjectSchema({{"folder", StringSchema(1, 2048, "Absolute path of the project folder.")},
                             {"name", StringSchema(1, 64, "Short project name: letters, digits, dots, dashes or underscores.")},
                             {"test_command", StringSchema(0, 512,
                                                  "Optional command that runs the tests inside the project, as an absolute guest "
                                                  "executable and arguments, for example '/usr/bin/python3 -m pytest -q'.")}},
                {"folder", "name"}),
            ToolEffect::Effect);
        tool.describe = [](const Json& arguments) { return "Sharing " + OneLine(Text(arguments, "folder")) + " with the sandbox"; };
        tool.execute = [share = executors.shareProject](const Json& arguments, const ToolCallContext& context)
        { return share(Text(arguments, "folder"), Text(arguments, "name"), Text(arguments, "test_command"), context); };
        registry.Add(std::move(tool));
    }
    // The project tools are useless when nothing can be shared and nothing has been.
    if (!permissions.share && permissions.projects.empty())
        return;
    const Json projectArgument = StringSchema(1, 64, "The shared project's name.");
    if (permissions.projectRead && executors.repoGrep)
    {
        auto tool = Tool("repo_grep",
            "Search a shared project's files in the sandbox. Returns matching lines as path:line:text, at most 100." + projects,
            ObjectSchema({{"project", projectArgument}, {"pattern", StringSchema(1, 256, "A Python regular expression, or plain text.")},
                             {"fixed_strings", {{"type", "boolean"}, {"description", "True to match the pattern as plain text."}}},
                             {"ignore_case", {{"type", "boolean"}, {"description", "True to ignore case."}}}},
                {"project", "pattern"}),
            ToolEffect::ReadOnly);
        tool.describe = [](const Json& arguments)
        { return "Searching " + OneLine(Text(arguments, "project"), 40) + " for \"" + OneLine(Text(arguments, "pattern")) + "\""; };
        tool.execute = [grep = executors.repoGrep](const Json& arguments, const ToolCallContext& context)
        {
            return grep(
                Text(arguments, "project"), Text(arguments, "pattern"), Flag(arguments, "fixed_strings"), Flag(arguments, "ignore_case"), context);
        };
        registry.Add(std::move(tool));
    }
    if (permissions.projectRead && executors.readFileRange)
    {
        auto tool = Tool("read_file_range",
            "Read numbered lines of a file in a shared project, at most 400 lines per call." + projects,
            ObjectSchema({{"project", projectArgument}, {"path", StringSchema(1, 1024, "Path inside the project, such as src/main.py.")},
                             {"start_line", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000000}}},
                             {"end_line", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000000}}}},
                {"project", "path", "start_line", "end_line"}),
            ToolEffect::ReadOnly);
        tool.describe = [](const Json& arguments)
        {
            return "Reading " + OneLine(Text(arguments, "path"), 60) + " lines " + std::to_string(Integer(arguments, "start_line", 1)) + "-" +
                   std::to_string(Integer(arguments, "end_line", 1));
        };
        tool.execute = [read = executors.readFileRange](const Json& arguments, const ToolCallContext& context)
        {
            return read(Text(arguments, "project"), Text(arguments, "path"), Integer(arguments, "start_line", 1),
                Integer(arguments, "end_line", 1), context);
        };
        registry.Add(std::move(tool));
    }
    if (permissions.projectWrite && executors.applyPatch)
    {
        auto tool = Tool("apply_patch",
            "Apply a unified diff (as git diff writes it, paths relative to the project) to the sandbox copy of a shared project. Each "
            "hunk must match the current file exactly; read the lines first. It can change and create files, not delete or rename "
            "them. The owner's own files are never changed: use export_patch to hand the result back." + projects,
            ObjectSchema({{"project", projectArgument}, {"patch", StringSchema(1, MaximumPatchArgumentBytes, "The unified diff.")}},
                {"project", "patch"}),
            ToolEffect::Effect);
        tool.maxArgumentBytes = MaximumPatchArgumentBytes + 1024;
        tool.describe = [](const Json& arguments) { return "Applying a patch to the sandbox copy of " + OneLine(Text(arguments, "project"), 40); };
        tool.execute = [apply = executors.applyPatch](const Json& arguments, const ToolCallContext& context)
        { return apply(Text(arguments, "project"), Text(arguments, "patch"), context); };
        registry.Add(std::move(tool));
    }
    if (permissions.projectRun && executors.runTests)
    {
        auto tool = Tool("run_tests",
            "Run the test command configured when the project was shared, inside its sandbox copy. The owner approves each run." + projects,
            ObjectSchema({{"project", projectArgument}}, {"project"}), ToolEffect::Effect);
        tool.describe = [](const Json& arguments) { return "Running the tests of " + OneLine(Text(arguments, "project"), 40); };
        tool.execute = [run = executors.runTests](const Json& arguments, const ToolCallContext& context)
        { return run(Text(arguments, "project"), context); };
        registry.Add(std::move(tool));
    }
    if (permissions.projectWrite && executors.exportPatch)
    {
        auto tool = Tool("export_patch",
            "Save every change made to a shared project's sandbox copy as one patch file for the owner to review and apply "
            "themselves. Nothing is applied to their files." + projects,
            ObjectSchema({{"project", projectArgument}}, {"project"}), ToolEffect::Effect);
        tool.describe = [](const Json& arguments) { return "Exporting the changes to " + OneLine(Text(arguments, "project"), 40); };
        tool.execute = [exportPatch = executors.exportPatch](const Json& arguments, const ToolCallContext& context)
        { return exportPatch(Text(arguments, "project"), context); };
        registry.Add(std::move(tool));
    }
}

// ---- Diff parsing, application and generation -----------------------------------------

bool ParseUnifiedDiff(const std::string& diff, std::vector<FilePatch>& files, std::string& error)
{
    files.clear();
    error.clear();
    std::vector<std::string> lines = Split(diff).lines;
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    FilePatch* file = nullptr;
    PatchHunk* hunk = nullptr;
    const auto fail = [&](std::string message)
    {
        files.clear();
        error = std::move(message);
        return false;
    };
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string& line = lines[index];
        if (hunk && (line.empty() || line[0] == ' ' || line[0] == '-' || line[0] == '+' || line[0] == '\\') && !StartsWith(line, "--- ") &&
            !StartsWith(line, "+++ "))
        {
            // A blank line inside a hunk is a blank context line whose space was dropped.
            hunk->lines.push_back(line.empty() ? std::string(" ") : line);
            file->text += line + "\n";
            continue;
        }
        hunk = nullptr;
        if (StartsWith(line, "deleted file mode") || (StartsWith(line, "+++ ") && DiffPath(line.substr(4)) == "/dev/null"))
            return fail("Deleting files is not supported; change their content instead.");
        if (StartsWith(line, "rename from") || StartsWith(line, "rename to") || StartsWith(line, "copy from"))
            return fail("Renaming or copying files is not supported.");
        if (StartsWith(line, "Binary files") || StartsWith(line, "GIT binary patch"))
            return fail("Binary patches are not supported.");
        if (StartsWith(line, "--- "))
        {
            if (index + 1 >= lines.size() || !StartsWith(lines[index + 1], "+++ "))
                return fail("A '--- ' line must be followed by a '+++ ' line.");
            std::string oldPath = DiffPath(line.substr(4));
            std::string newPath = DiffPath(lines[index + 1].substr(4));
            if (newPath == "/dev/null")
                return fail("Deleting files is not supported; change their content instead.");
            const bool created = oldPath == "/dev/null";
            if (StartsWith(oldPath, "a/"))
                oldPath = oldPath.substr(2);
            if (StartsWith(newPath, "b/"))
                newPath = newPath.substr(2);
            if (!created && oldPath != newPath)
                return fail("Renaming files is not supported (" + OneLine(oldPath) + " to " + OneLine(newPath) + ").");
            if (!policy::IsGuestRelativePath(newPath))
                return fail("The path " + OneLine(newPath) + " is not a relative path inside the project.");
            if (std::any_of(files.begin(), files.end(), [&newPath](const FilePatch& item) { return item.path == newPath; }))
                return fail("The patch names " + OneLine(newPath) + " twice; give each file one section.");
            if (files.size() >= MaximumPatchFiles)
                return fail("A patch may change at most " + std::to_string(MaximumPatchFiles) + " files.");
            files.push_back({newPath, created, line + "\n" + lines[index + 1] + "\n", {}});
            file = &files.back();
            ++index;
            continue;
        }
        std::size_t oldStart = 0;
        if (StartsWith(line, "@@"))
        {
            if (!file)
                return fail("A hunk appeared before any '--- a/path' and '+++ b/path' header.");
            if (!ParseHunkStart(line, oldStart))
                return fail("Unreadable hunk header: " + OneLine(line));
            file->hunks.push_back({oldStart, {}});
            hunk = &file->hunks.back();
            file->text += line + "\n";
            continue;
        }
        // "diff --git", "index ..." and any explanation around the diff carry nothing to apply.
    }
    if (files.empty())
        return fail("No file sections were found. Use '--- a/path', '+++ b/path' and '@@' hunks.");
    for (const auto& item : files)
    {
        const bool changes = std::any_of(item.hunks.begin(), item.hunks.end(), [](const PatchHunk& value)
            { return std::any_of(value.lines.begin(), value.lines.end(), [](const std::string& text) { return text[0] == '-' || text[0] == '+'; }); });
        if (!changes)
            return fail("The section for " + OneLine(item.path) + " changes nothing.");
    }
    return true;
}

bool ApplyFilePatch(const std::optional<std::string>& original, const FilePatch& patch, std::string& result, std::string& error)
{
    result.clear();
    error.clear();
    if (patch.created && original)
    {
        error = patch.path + " already exists; patch its content instead of creating it.";
        return false;
    }
    if (!patch.created && !original)
    {
        error = patch.path + " does not exist in the project.";
        return false;
    }
    Lines file = original ? Split(*original) : Lines{};
    std::ptrdiff_t delta = 0;
    std::size_t searchFrom = 0;
    for (std::size_t number = 0; number < patch.hunks.size(); ++number)
    {
        const auto& hunk = patch.hunks[number];
        std::vector<std::string> before;
        std::vector<std::string> after;
        bool endsWithoutNewline = false;
        bool marked = false;
        char previous = 0;
        for (const auto& line : hunk.lines)
        {
            const char kind = line[0];
            if (kind == '\\')
            {
                marked = true;
                endsWithoutNewline = previous == '+' || previous == ' ';
                continue;
            }
            std::string content = line.substr(1);
            if (!content.empty() && content.back() == '\r')
                content.pop_back();
            if (kind != '+')
                before.push_back(content);
            if (kind != '-')
                after.push_back(content);
            previous = kind;
        }
        const std::ptrdiff_t stated = static_cast<std::ptrdiff_t>(hunk.oldStart == 0 ? 0 : hunk.oldStart - 1) + delta;
        const auto matches = [&](const std::size_t at)
        {
            if (at < searchFrom || at + before.size() > file.lines.size())
                return false;
            return std::equal(before.begin(), before.end(), file.lines.begin() + static_cast<std::ptrdiff_t>(at));
        };
        std::optional<std::size_t> position;
        const std::size_t limit = file.lines.size() + 1;
        const std::size_t expected = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(stated, 0, static_cast<std::ptrdiff_t>(file.lines.size())));
        if (before.empty())
            position = std::max(expected, searchFrom);
        for (std::size_t distance = 0; !position && distance <= limit; ++distance)
        {
            if (expected + distance < limit && matches(expected + distance))
                position = expected + distance;
            else if (distance > 0 && distance <= expected && matches(expected - distance))
                position = expected - distance;
        }
        if (!position || *position > file.lines.size())
        {
            error = "Hunk " + std::to_string(number + 1) + " of " + patch.path + " does not match the file (near line " +
                    std::to_string(expected + 1) + "). Read the current lines and write the hunk from them.";
            return false;
        }
        const auto at = file.lines.begin() + static_cast<std::ptrdiff_t>(*position);
        file.lines.erase(at, at + static_cast<std::ptrdiff_t>(before.size()));
        file.lines.insert(file.lines.begin() + static_cast<std::ptrdiff_t>(*position), after.begin(), after.end());
        delta += static_cast<std::ptrdiff_t>(after.size()) - static_cast<std::ptrdiff_t>(before.size());
        searchFrom = *position + after.size();
        if (patch.created || (marked && searchFrom == file.lines.size()))
            file.finalNewline = !endsWithoutNewline;
    }
    result = Join(file);
    return true;
}

std::string UnifiedDiff(const std::string& path, const std::optional<std::string>& before, const std::string& after)
{
    const Lines oldLines = before ? Split(*before) : Lines{};
    const Lines newLines = Split(after);
    const auto script = EditScript(Tokens(oldLines), Tokens(newLines));
    if (std::all_of(script.begin(), script.end(), [](const auto& step) { return step.first == Edit::Same; }) && before)
        return {};
    constexpr std::size_t Context = 3;
    std::string text = "--- " + (before ? "a/" + path : std::string("/dev/null")) + "\n+++ b/" + path + "\n";
    std::size_t index = 0;
    std::size_t oldLine = 0;
    std::size_t newLine = 0;
    while (index < script.size())
    {
        // The next change, and the context window around it and any change close behind it.
        std::size_t change = index;
        std::size_t skippedOld = 0;
        std::size_t skippedNew = 0;
        while (change < script.size() && script[change].first == Edit::Same)
        {
            ++change;
            ++skippedOld;
            ++skippedNew;
        }
        if (change == script.size())
            break;
        const std::size_t lead = std::min(Context, change - index);
        const std::size_t start = change - lead;
        const std::size_t hunkOld = oldLine + skippedOld - lead;
        const std::size_t hunkNew = newLine + skippedNew - lead;
        // Changes closer than twice the context share one hunk.
        std::size_t end = change;
        while (end < script.size())
        {
            std::size_t run = 0;
            while (end + run < script.size() && script[end + run].first == Edit::Same)
                ++run;
            if (end + run == script.size() || run > Context * 2)
            {
                end += std::min(run, Context);
                break;
            }
            end += run;
            while (end < script.size() && script[end].first != Edit::Same)
                ++end;
        }
        std::size_t oldCount = 0;
        std::size_t newCount = 0;
        std::string body;
        for (std::size_t step = start; step < end; ++step)
        {
            const auto& [kind, value] = script[step];
            const bool missingNewline = value.size() >= NoNewline.size() &&
                                        value.compare(value.size() - NoNewline.size(), NoNewline.size(), NoNewline) == 0;
            const std::string line = missingNewline ? value.substr(0, value.size() - NoNewline.size()) : value;
            body += (kind == Edit::Same ? ' ' : kind == Edit::Remove ? '-' : '+') + line + "\n";
            if (missingNewline)
                body += "\\ No newline at end of file\n";
            if (kind != Edit::Add)
                ++oldCount;
            if (kind != Edit::Remove)
                ++newCount;
        }
        text += "@@ -" + std::to_string(oldCount == 0 ? hunkOld : hunkOld + 1) + "," + std::to_string(oldCount) + " +" +
                std::to_string(newCount == 0 ? hunkNew : hunkNew + 1) + "," + std::to_string(newCount) + " @@\n" + body;
        for (std::size_t step = index; step < end; ++step)
        {
            if (script[step].first != Edit::Add)
                ++oldLine;
            if (script[step].first != Edit::Remove)
                ++newLine;
        }
        index = end;
    }
    return text;
}

std::string NumberedLines(const std::string& text, const int first, const int last, const std::size_t maximumLines)
{
    const Lines file = Split(text);
    if (file.lines.empty())
        return "(the file is empty)";
    const std::size_t from = static_cast<std::size_t>(std::max(first, 1));
    if (from > file.lines.size())
        return "(the file has only " + std::to_string(file.lines.size()) + " lines)";
    std::size_t to = std::min(static_cast<std::size_t>(std::max(last, first)), file.lines.size());
    std::string shown;
    if (to - from + 1 > maximumLines)
        to = from + maximumLines - 1;
    for (std::size_t line = from; line <= to; ++line)
        shown += std::to_string(line) + "| " + file.lines[line - 1] + "\n";
    if (to < file.lines.size())
        shown += "(" + std::to_string(file.lines.size()) + " lines in all)\n";
    return shown;
}

// ---- The session's workspace -----------------------------------------------------------

SandboxWorkspace::SandboxWorkspace(std::filesystem::path root) : artifactRoot(std::move(root))
{
}

const std::filesystem::path& SandboxWorkspace::ArtifactRoot() const
{
    return artifactRoot;
}

std::vector<std::string> SandboxWorkspace::Projects() const
{
    std::lock_guard lock(mutex);
    std::vector<std::string> names;
    for (const auto& entry : projects)
        names.push_back(entry.first);
    return names;
}

std::optional<SandboxWorkspace::Project> SandboxWorkspace::Find(const std::string& name) const
{
    std::lock_guard lock(mutex);
    const auto found = projects.find(name);
    return found == projects.end() ? std::nullopt : std::optional<Project>(found->second);
}

SandboxToolExecutors SandboxWorkspace::Bind(Binding input)
{
    auto binding = std::make_shared<const Binding>(std::move(input));
    SandboxToolExecutors executors;
    executors.runPython = [this, binding](const std::string& code, const int seconds, const ToolCallContext& context)
    { return RunPython(*binding, code, seconds, context); };
    executors.shareProject = [this, binding](const std::string& folder, const std::string& name, const std::string& command,
                                 const ToolCallContext& context) { return ShareProject(*binding, folder, name, command, context); };
    executors.repoGrep = [this, binding](const std::string& project, const std::string& pattern, const bool fixed, const bool ignoreCase,
                             const ToolCallContext& context) { return RepoGrep(*binding, project, pattern, fixed, ignoreCase, context); };
    executors.readFileRange = [this, binding](const std::string& project, const std::string& path, const int first, const int last,
                                  const ToolCallContext& context) { return ReadFileRange(*binding, project, path, first, last, context); };
    executors.applyPatch = [this, binding](const std::string& project, const std::string& patch, const ToolCallContext& context)
    { return ApplyPatch(*binding, project, patch, context); };
    executors.runTests = [this, binding](const std::string& project, const ToolCallContext& context)
    { return RunTests(*binding, project, context); };
    executors.exportPatch = [this, binding](const std::string& project, const ToolCallContext& context)
    { return ExportPatch(*binding, project, context); };
    return executors;
}

namespace
{
// One guest action through the turn's port, with its task contract. Nothing when it was
// refused before the runtime; `refusal` then says why.
std::optional<actions::ActionOutcome> Submit(
    const SandboxWorkspace::Binding& binding, actions::ActionRequest request, const ToolCallContext& context, std::string& refusal)
{
    if (binding.prepare && !binding.prepare(request))
    {
        refusal = "The turn's task contract is no longer current.";
        return std::nullopt;
    }
    return SubmitChatAction(binding.port, std::move(request), context, refusal);
}

std::string Failure(const actions::ActionOutcome& outcome)
{
    const std::string message = outcome.Message();
    return message.empty() ? outcome.policy.reason : message;
}

struct GuestText
{
    bool exists = false;
    std::string content;
    std::string digest;
};

// Reads one file of a project's guest copy; `error` explains a failure.
std::optional<GuestText> ReadGuestFile(const SandboxWorkspace::Binding& binding, const std::string& project, const std::string& path,
    const ToolCallContext& context, std::string& error)
{
    auto request = GuestRequest(actions::ActionType::VmReadTextFile, GuestProjectPath(project, path));
    const auto outcome = Submit(binding, std::move(request), context, error);
    if (!outcome)
        return std::nullopt;
    if (!outcome->Succeeded() || !outcome->result.guest)
    {
        error = "Could not read " + path + ": " + Failure(*outcome);
        return std::nullopt;
    }
    return GuestText{true, outcome->result.guest->content, outcome->result.guest->digest};
}
}

ToolExecution SandboxWorkspace::RunPython(const Binding& binding, const std::string& code, const int timeoutSeconds, const ToolCallContext& context)
{
    auto request = GuestRequest(actions::ActionType::VmRunPython);
    request.guest.runId = RunId();
    request.guest.code = code;
    request.guest.timeoutMs = std::min(std::clamp(timeoutSeconds, 1, 120) * 1000, binding.maxTimeoutMs);
    std::string refusal;
    const auto outcome = Submit(binding, request, context, refusal);
    if (!outcome)
        return ToolExecution::Fail(refusal);
    const auto& receipt = outcome->result.guest;
    // A run without a verified receipt proves nothing, not even that its output is its own.
    if (!receipt || !receipt->attempted || !receipt->receiptVerified || !outcome->auditError.empty())
        return ToolExecution::Fail("The run was not completed: " + Failure(*outcome));
    std::string text = DescribeRun(*receipt, request.guest.timeoutMs);
    std::vector<std::string> notes;
    const auto saved = SaveArtifacts(request.guest.runId, receipt->artifacts, notes);
    for (const auto& path : saved)
        text += "\nfile: " + actions::PathToUtf8(path.filename()) + " saved to " + actions::PathToUtf8(path);
    for (const auto& note : notes)
        text += "\nfile not kept: " + note;
    for (const auto& omitted : receipt->artifactsOmitted)
        text += "\nfile not copied back: " + OneLine(omitted, 160);
    const auto image = std::find_if(saved.begin(), saved.end(), [](const std::filesystem::path& path)
        {
            const std::string extension = Lower(actions::PathToUtf8(path.extension()));
            return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".gif";
        });
    if (image != saved.end() && binding.showImage)
    {
        binding.showImage(*image);
        text += "\n" + actions::PathToUtf8(image->filename()) + " is shown on the Canvas tab.";
    }
    return ToolExecution::Ok(std::move(text));
}

std::vector<std::filesystem::path> SandboxWorkspace::SaveArtifacts(
    const std::string& runId, const std::vector<workspace::GuestArtifact>& artifacts, std::vector<std::string>& notes)
{
    std::vector<std::filesystem::path> saved;
    if (artifacts.empty())
        return saved;
    std::error_code error;
    const auto runs = artifactRoot / "runs";
    std::filesystem::create_directories(runs, error);
    const auto folder = runs / runId;
    if (error || !std::filesystem::create_directory(folder, error) || error)
    {
        notes.push_back("the artifact folder could not be created");
        return saved;
    }
    for (const auto& artifact : artifacts)
    {
        std::string extension;
        if (!ArtifactNameAllowed(artifact.name, extension) || artifact.bytes.size() > workspace::MaxGuestArtifactBytes)
        {
            notes.push_back(OneLine(artifact.name, 104) + ": not an allowed file name or type");
            continue;
        }
        if (!ImageMatches(extension, artifact.bytes))
        {
            notes.push_back(artifact.name + ": its content is not what its name says");
            continue;
        }
        const auto path = folder / actions::Utf8ToPath(artifact.name);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(artifact.bytes.data(), static_cast<std::streamsize>(artifact.bytes.size()));
        stream.close();
        if (stream.fail())
        {
            notes.push_back(artifact.name + ": could not be written");
            std::filesystem::remove(path, error);
            continue;
        }
        saved.push_back(path);
    }
    PruneOldest(runs, "run-", RetainedRuns);
    return saved;
}

ToolExecution SandboxWorkspace::ShareProject(
    const Binding& binding, const std::string& folder, const std::string& name, const std::string& testCommand, const ToolCallContext& context)
{
    if (!policy::IsGuestProjectName(name))
        return ToolExecution::Fail("The project name must be 1 to 64 letters, digits, dots, dashes or underscores, not starting with one of "
                                   "the last three.");
    std::vector<std::string> command;
    std::istringstream words(testCommand);
    for (std::string word; words >> word;)
        command.push_back(word);
    if (!command.empty() && (command.front().front() != '/' || command.size() > 32))
        return ToolExecution::Fail("The test command must start with an absolute guest executable, such as /usr/bin/python3.");
    auto request = GuestRequest(actions::ActionType::VmShareProject, "projects/.incoming-" + RunId().substr(4));
    request.source = actions::Utf8ToPath(folder);
    request.guest.publish = name;
    request.guest.review = command.empty() ? std::string("No test command.") : "Test command for run_tests: " + testCommand;
    std::string refusal;
    const auto outcome = Submit(binding, std::move(request), context, refusal);
    if (!outcome)
        return ToolExecution::Fail(refusal);
    if (!outcome->Succeeded())
        return ToolExecution::Fail(Failure(*outcome));
    {
        std::lock_guard lock(mutex);
        // A new share replaces the guest copy, so earlier patches no longer apply to it.
        projects[name] = Project{outcome->policy.canonicalSource, command, {}};
    }
    std::string text = outcome->result.message;
    for (const auto& skipped : outcome->result.entries)
        text += "\nleft out: " + skipped;
    text += "\nUse repo_grep, read_file_range, apply_patch" + std::string(command.empty() ? "" : ", run_tests") +
            " and export_patch with project \"" + name + "\".";
    return ToolExecution::Ok(std::move(text));
}

ToolExecution SandboxWorkspace::RepoGrep(const Binding& binding, const std::string& project, const std::string& pattern, const bool fixedStrings,
    const bool ignoreCase, const ToolCallContext& context)
{
    if (!Find(project))
        return ToolExecution::Fail("No project named \"" + OneLine(project, 64) + "\" is shared in this session; use share_project first.");
    auto request = GuestRequest(actions::ActionType::VmSearchFiles, GuestProjectPath(project));
    request.guest.pattern = pattern;
    request.guest.fixedStrings = fixedStrings;
    request.guest.ignoreCase = ignoreCase;
    request.guest.timeoutMs = std::min(SearchTimeoutMs, binding.maxTimeoutMs);
    std::string refusal;
    const auto outcome = Submit(binding, std::move(request), context, refusal);
    if (!outcome)
        return ToolExecution::Fail(refusal);
    const auto& receipt = outcome->result.guest;
    if (!outcome->Succeeded() || !receipt)
    {
        const std::string reason = receipt && !receipt->standardError.empty() ? Tail(receipt->standardError, 400)
                                   : receipt && receipt->timedOut               ? std::string("The search took too long; use a simpler pattern.")
                                                                                : Failure(*outcome);
        return ToolExecution::Fail(reason);
    }
    return ToolExecution::Ok(receipt->standardOutput.empty() ? std::string("No matches.") : receipt->standardOutput);
}

ToolExecution SandboxWorkspace::ReadFileRange(
    const Binding& binding, const std::string& project, const std::string& path, const int firstLine, const int lastLine, const ToolCallContext& context)
{
    if (!Find(project))
        return ToolExecution::Fail("No project named \"" + OneLine(project, 64) + "\" is shared in this session; use share_project first.");
    if (!policy::IsGuestRelativePath(path))
        return ToolExecution::Fail("The path must be relative to the project, such as src/main.py.");
    if (lastLine < firstLine)
        return ToolExecution::Fail("end_line must not be before start_line.");
    std::string error;
    const auto file = ReadGuestFile(binding, project, path, context, error);
    if (!file)
        return ToolExecution::Fail(error);
    return ToolExecution::Ok(path + ":\n" + NumberedLines(file->content, firstLine, lastLine, MaximumReadLines));
}

ToolExecution SandboxWorkspace::ApplyPatch(const Binding& binding, const std::string& project, const std::string& patch, const ToolCallContext& context)
{
    if (!Find(project))
        return ToolExecution::Fail("No project named \"" + OneLine(project, 64) + "\" is shared in this session; use share_project first.");
    std::vector<FilePatch> files;
    std::string error;
    if (!ParseUnifiedDiff(patch, files, error))
        return ToolExecution::Fail(error);

    // Every file is read and patched in memory first, so a hunk that does not match changes nothing.
    struct Planned
    {
        const FilePatch* patch = nullptr;
        std::optional<std::string> before;
        std::string digest;
        std::string after;
    };
    std::vector<Planned> planned;
    for (const auto& file : files)
    {
        Planned item{&file, std::nullopt, "missing", {}};
        if (!file.created)
        {
            const auto current = ReadGuestFile(binding, project, file.path, context, error);
            if (!current)
                return ToolExecution::Fail(error + " Nothing was changed.");
            item.before = current->content;
            item.digest = current->digest;
        }
        if (!ApplyFilePatch(item.before, file, item.after, error))
            return ToolExecution::Fail(error + " Nothing was changed.");
        planned.push_back(std::move(item));
    }

    std::string summary;
    std::size_t written = 0;
    for (const auto& item : planned)
    {
        const std::string diff = UnifiedDiff(item.patch->path, item.before, item.after);
        auto request = GuestRequest(actions::ActionType::VmWriteTextFile, GuestProjectPath(project, item.patch->path));
        request.guest.content = item.after;
        request.guest.expectedDigest = item.digest;
        // What the owner approves is the change, not the whole new file.
        request.guest.review = utf8::Prefix(diff, 60000);
        std::string refusal;
        const auto outcome = Submit(binding, std::move(request), context, refusal);
        if (!outcome || !outcome->Succeeded())
        {
            const std::string reason = outcome ? Failure(*outcome) : refusal;
            return ToolExecution::Fail("Stopped at " + item.patch->path + ": " + reason +
                                       (written == 0 ? " Nothing was changed." : " Earlier files in this patch were changed:" + summary));
        }
        ++written;
        {
            std::lock_guard lock(mutex);
            auto found = projects.find(project);
            // The first version this session saw is what an export compares against.
            if (found != projects.end())
                found->second.baseline.try_emplace(item.patch->path, item.before);
        }
        std::size_t plus = 0;
        std::size_t minus = 0;
        std::istringstream lines(diff);
        for (std::string line; std::getline(lines, line);)
        {
            if (StartsWith(line, "+") && !StartsWith(line, "+++"))
                ++plus;
            else if (StartsWith(line, "-") && !StartsWith(line, "---"))
                ++minus;
        }
        summary += " " + item.patch->path + " (+" + std::to_string(plus) + " -" + std::to_string(minus) + ")";
        if (binding.showPatch)
            binding.showPatch("Patch applied in the sandbox copy of " + project + ": " + item.patch->path, diff);
    }
    return ToolExecution::Ok("Applied to the sandbox copy of " + project + ":" + summary +
                             ". The owner's own files are unchanged; export_patch saves the changes for them.");
}

ToolExecution SandboxWorkspace::RunTests(const Binding& binding, const std::string& project, const ToolCallContext& context)
{
    const auto found = Find(project);
    if (!found)
        return ToolExecution::Fail("No project named \"" + OneLine(project, 64) + "\" is shared in this session; use share_project first.");
    if (found->testCommand.empty())
        return ToolExecution::Fail("No test command was set when " + project + " was shared; share it again with test_command.");
    auto request = GuestRequest(actions::ActionType::VmExecuteProcess);
    request.guest.executable = found->testCommand.front();
    request.guest.arguments.assign(found->testCommand.begin() + 1, found->testCommand.end());
    request.guest.workingDirectory = GuestProjectPath(project);
    request.guest.timeoutMs = std::min(TestTimeoutMs, binding.maxTimeoutMs);
    std::string refusal;
    const auto outcome = Submit(binding, request, context, refusal);
    if (!outcome)
        return ToolExecution::Fail(refusal);
    const auto& receipt = outcome->result.guest;
    if (!receipt || !receipt->attempted || !receipt->receiptVerified || !outcome->auditError.empty())
        return ToolExecution::Fail("The tests did not run to a verified end: " + Failure(*outcome));
    return ToolExecution::Ok((receipt->exitCode == 0 ? "The test command passed.\n" : "The test command failed.\n") +
                             DescribeRun(*receipt, request.guest.timeoutMs));
}

ToolExecution SandboxWorkspace::ExportPatch(const Binding& binding, const std::string& project, const ToolCallContext& context)
{
    const auto found = Find(project);
    if (!found)
        return ToolExecution::Fail("No project named \"" + OneLine(project, 64) + "\" is shared in this session; use share_project first.");
    std::string patch;
    std::size_t changed = 0;
    for (const auto& [path, before] : found->baseline)
    {
        std::string error;
        const auto current = ReadGuestFile(binding, project, path, context, error);
        if (!current)
            return ToolExecution::Fail(error + " No patch was saved.");
        const std::string diff = UnifiedDiff(path, before, current->content);
        if (diff.empty())
            continue;
        patch += diff;
        ++changed;
        if (patch.size() > MaximumPatchBytes)
            return ToolExecution::Fail("The changes exceed " + std::to_string(MaximumPatchBytes) + " bytes; no patch was saved.");
    }
    if (patch.empty())
        return ToolExecution::Ok("There are no changes to " + project + " since it was shared.");
    std::error_code error;
    const auto folder = artifactRoot / "patches";
    std::filesystem::create_directories(folder, error);
    const auto path = folder / actions::Utf8ToPath(project + "-" + RunId().substr(4) + ".patch");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << patch;
        stream.close();
        if (error || stream.fail())
            return ToolExecution::Fail("The patch could not be saved under " + actions::PathToUtf8(folder) + ".");
    }
    if (binding.showPatch)
        binding.showPatch("Patch for " + project + " saved to " + actions::PathToUtf8(path), patch);
    const std::string file = actions::PathToUtf8(path);
    return ToolExecution::Ok("Saved the changes to " + std::to_string(changed) + (changed == 1 ? " file" : " files") + " as " + file +
                             ". Nothing was applied to the owner's files. To apply it, review the patch, then run in " +
                             actions::PathToUtf8(found->folder) + ":\ngit apply --check \"" + file + "\"\ngit apply \"" + file + "\"");
}

} // namespace revia::agents
