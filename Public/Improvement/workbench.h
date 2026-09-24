#pragma once

#include "Improvement/codeProposal.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::improvement
{

// One CTest suite and whether it passed.
struct TestOutcome
{
    std::string name;
    bool passed = false;
};

// CTest's own summary lines ("1/10 Test #1: Revia.Foundation ... Passed 74.9 sec").
[[nodiscard]] std::vector<TestOutcome> ParseCtestOutput(const std::string& output);

// Compiler and linker errors from a build log, first ones first. What a repair attempt is
// shown, so it is bounded and keeps only the lines that say what is wrong.
[[nodiscard]] std::string ExtractBuildErrors(const std::string& log, std::size_t maximumLines = 20);

// What one build-and-test run produced.
struct BuildOutcome
{
    // False when the run itself did not finish: cancelled, timed out, or the toolchain
    // could not be started. Nothing about the change can be concluded from that.
    bool completed = false;
    bool built = false;
    bool testsRan = false;
    std::string testOutput;
    std::string buildLog;
    std::string failure;
    double seconds = 0.0;
};

using BuildRunner = std::function<BuildOutcome(
    const std::filesystem::path& sourceRoot,
    const std::filesystem::path& buildRoot,
    std::stop_token stopToken)>;

struct VerificationResult
{
    // Finished with a verdict. False: cancelled or could not run, so try again later.
    bool concluded = false;
    bool verified = false;
    // One line for the proposal record.
    std::string summary;
    // Compiler errors, when it did not build, for one repair attempt.
    std::string buildErrors;
    double seconds = 0.0;
};

// A private copy of her source where a proposal is proven.
//
// "Proven" means: the copy builds with the change, and no test suite that passes without
// the change fails with it. The real source is never written. Every write this class
// makes is checked to land inside its own root, and the change is reverted in the copy
// once the run is over, whatever the outcome.
//
// It lives outside the synced documents folder on purpose: a build tree is hundreds of
// megabytes of files rewritten constantly, which a sync client locks and corrupts.
class Workbench
{
public:
    Workbench(std::filesystem::path sourceRoot, std::filesystem::path workbenchRoot,
        BuildRunner runner);

    [[nodiscard]] const std::filesystem::path& Root() const { return root; }
    [[nodiscard]] std::filesystem::path MirrorRoot() const { return root / "src"; }
    [[nodiscard]] std::filesystem::path BuildRoot() const { return root / "build"; }

    // Brings the copy level with the source: copies what changed since the last sync and
    // removes what the source no longer has. Returns the number of files that changed.
    std::size_t Sync(std::string& outError);

    [[nodiscard]] VerificationResult Verify(const CodeChange& change, std::stop_token stopToken);

    // Only these reach the copy: what the build reads, nothing it produces.
    [[nodiscard]] static bool IsMirrored(const std::string& relativePath);

private:
    [[nodiscard]] bool Inside(const std::filesystem::path& path) const;
    bool WriteMirrorFile(const std::string& relativePath, const std::string& content,
        std::string& outError) const;
    void LoadManifest();
    void SaveManifest() const;
    [[nodiscard]] std::string ManifestStamp() const;

    std::filesystem::path source;
    std::filesystem::path root;
    BuildRunner runner;
    // Source file -> "size:mtime" when it was last copied.
    std::map<std::string, std::string> manifest;
    // Suites failing in the unchanged copy, and the source state they were measured on.
    std::set<std::string> baselineFailures;
    std::string baselineStamp;
};

// The real runner: Tools/VerifyWorkbench.ps1 at below-normal priority, in a job object
// so a cancel stops the whole compiler tree, output to `logPath`.
[[nodiscard]] BuildRunner MakeScriptRunner(
    std::filesystem::path scriptPath,
    std::filesystem::path dependencyRoot,
    std::filesystem::path logDirectory,
    int parallelJobs,
    int timeoutMinutes);

} // namespace revia::improvement
