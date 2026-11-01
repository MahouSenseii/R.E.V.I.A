#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace revia::runtime
{

struct TestRunConfig
{
    bool enabled = false;
    std::filesystem::path sourceRoot;
    std::filesystem::path buildRoot;
    std::filesystem::path ctestExecutable;
    std::filesystem::path policyFile;
    std::filesystem::path outputRoot;
    // A developer claim, recorded alongside computed source/build fingerprints.
    std::string sourceIdentity;
    std::string configuration;
    std::map<std::string, std::string> environment;
};

struct TestCatalogEntry
{
    std::string id;
    std::string purpose;
    std::string group;
    std::vector<std::string> groups;
    std::string scope;
    std::vector<std::string> prerequisites;
    std::vector<std::string> labels;
    std::string availability;
    std::string latestOutcome = "unverified";
    std::string applicability = "unverified";
    double timeoutSeconds = 0;
};

struct TestModuleResult
{
    std::string id;
    std::string outcome;
    std::string detail;
    std::int64_t elapsedMs = 0;
    std::int64_t exitCode = -1;
    bool fresh = false;
    std::filesystem::path logFile;
    std::filesystem::path junitFile;
    std::string observedAt;
    std::string reusedFrom;
};

struct TestRunSnapshot
{
    std::string phase = "disabled";
    std::string message;
    std::string catalogIdentity;
    std::string runId;
    std::filesystem::path outputDirectory;
    std::string activeTest;
    std::vector<TestCatalogEntry> catalog;
    std::vector<TestModuleResult> results;
};

class TestRunRuntime
{
  public:
    TestRunRuntime();
    explicit TestRunRuntime(const TestRunConfig& config);
    ~TestRunRuntime();
    TestRunRuntime(const TestRunRuntime&) = delete;
    TestRunRuntime& operator=(const TestRunRuntime&) = delete;

    [[nodiscard]] bool Configure(const TestRunConfig& config, std::string& error);
    [[nodiscard]] bool Discover(std::string& error);
    [[nodiscard]] bool Start(const std::string& group, std::string& error, const std::filesystem::path& resumeFrom = {});
    [[nodiscard]] TestRunSnapshot Snapshot() const;
    void Cancel();
    void Shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

[[nodiscard]] std::string ReadTestRunCheckpoint(const std::filesystem::path& outputRoot, const std::filesystem::path& directory);

} // namespace revia::runtime
