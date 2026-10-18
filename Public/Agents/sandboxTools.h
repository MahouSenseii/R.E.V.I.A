#pragma once

#include "Agents/chatTools.h"
#include "Agents/toolRegistry.h"
#include "Policy/guestCapabilityPolicy.h"
#include "Workspace/virtualMachineRuntime.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::agents
{

// What the guest code tools may offer this turn, from the same guest permission and VM state
// that govern every guest action. It never widens either. `guestState` is null when no VM is
// configured to start.
[[nodiscard]] SandboxToolPermissions ProjectSandboxTools(const policy::GuestCapabilitySettings& guest,
    const workspace::VirtualMachineSnapshot* guestState, const std::vector<std::filesystem::path>& approvedRoots,
    std::vector<std::string> projects);

// One hunk of a unified diff. `lines` keep their ' ', '-', '+' or '\' prefix.
struct PatchHunk
{
    std::size_t oldStart = 0;
    std::vector<std::string> lines;
};

// One file's part of a unified diff: its path relative to the project, whether the diff
// creates it, and its own text for review.
struct FilePatch
{
    std::string path;
    bool created = false;
    std::string text;
    std::vector<PatchHunk> hunks;
};

// Reads a unified diff as `git diff` or `diff -u` writes it. Hunk line counts are not
// trusted, since models often get them wrong; a hunk ends where its prefixed lines end.
// Renames, deletions and binary patches are refused, and so is any path that is not a
// normalized relative path.
[[nodiscard]] bool ParseUnifiedDiff(const std::string& diff, std::vector<FilePatch>& files, std::string& error);

// Applies one file's hunks in order. Each hunk must match the file exactly, at its stated
// line or the nearest place after the previous hunk; line endings (LF or CRLF) are kept.
[[nodiscard]] bool ApplyFilePatch(const std::optional<std::string>& original, const FilePatch& patch, std::string& result, std::string& error);

// A unified diff from `before` (nullopt for a new file) to `after`, with three lines of
// context, that `git apply` accepts. Empty when nothing changed.
[[nodiscard]] std::string UnifiedDiff(const std::string& path, const std::optional<std::string>& before, const std::string& after);

// Lines first..last of a text, each numbered, at most `maximumLines` of them.
[[nodiscard]] std::string NumberedLines(const std::string& text, int first, int last, std::size_t maximumLines);

// The code tools' state for one session: the projects shared into the guest, what each file
// looked like before the first patch changed it, and the host folder where generated files
// and exported patches are kept. That folder belongs to Revia, like her drawings: the tools
// never write into an approved root or into the project's own source.
class SandboxWorkspace
{
  public:
    // How one turn reaches the world: the action port with its policy, confirmation and
    // audit, and optional ways to show a picture or a patch beside the chat.
    struct Binding
    {
        ChatActionPort port;
        // Stamps the turn's task contract on a request; false when the turn is not current.
        std::function<bool(actions::ActionRequest&)> prepare;
        std::function<void(const std::filesystem::path&)> showImage;
        std::function<void(const std::string& title, const std::string& diff)> showPatch;
        // The guest permission's process deadline, so no request asks for more than policy allows.
        int maxTimeoutMs = 120000;
    };

    // Kept per kind of file under the artifact root.
    static constexpr std::size_t RetainedRuns = 20;
    static constexpr std::size_t MaximumPatchBytes = 1u << 20;

    explicit SandboxWorkspace(std::filesystem::path artifactRoot);
    SandboxWorkspace(const SandboxWorkspace&) = delete;
    SandboxWorkspace& operator=(const SandboxWorkspace&) = delete;

    // Executors bound to one turn's binding. They hold this workspace by reference, so the
    // workspace must outlive the turn, as the session's does.
    [[nodiscard]] SandboxToolExecutors Bind(Binding binding);
    [[nodiscard]] std::vector<std::string> Projects() const;
    [[nodiscard]] const std::filesystem::path& ArtifactRoot() const;

  private:
    struct Project
    {
        std::filesystem::path folder;
        std::vector<std::string> testCommand;
        // Content before the first patch touched a file; nullopt when the patch created it.
        std::map<std::string, std::optional<std::string>> baseline;
    };

    ToolExecution RunPython(const Binding& binding, const std::string& code, int timeoutSeconds, const ToolCallContext& context);
    ToolExecution ShareProject(const Binding& binding, const std::string& folder, const std::string& name, const std::string& testCommand,
        const ToolCallContext& context);
    ToolExecution RepoGrep(const Binding& binding, const std::string& project, const std::string& pattern, bool fixedStrings, bool ignoreCase,
        const ToolCallContext& context);
    ToolExecution ReadFileRange(
        const Binding& binding, const std::string& project, const std::string& path, int firstLine, int lastLine, const ToolCallContext& context);
    ToolExecution ApplyPatch(const Binding& binding, const std::string& project, const std::string& patch, const ToolCallContext& context);
    ToolExecution RunTests(const Binding& binding, const std::string& project, const ToolCallContext& context);
    ToolExecution ExportPatch(const Binding& binding, const std::string& project, const ToolCallContext& context);

    [[nodiscard]] std::optional<Project> Find(const std::string& name) const;
    [[nodiscard]] std::vector<std::filesystem::path> SaveArtifacts(
        const std::string& runId, const std::vector<workspace::GuestArtifact>& artifacts, std::vector<std::string>& notes);

    mutable std::mutex mutex;
    std::filesystem::path artifactRoot;
    std::map<std::string, Project> projects;
};

} // namespace revia::agents
