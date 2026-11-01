#include "Runtime/testRunRuntime.h"
#include "Audit/contentDigest.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using revia::runtime::TestRunRuntime;
namespace fs = std::filesystem;

void Check(const bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Write(const fs::path& file, const std::string& bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary);
    stream << bytes;
    Check(static_cast<bool>(stream), "Could not write disposable fixture.");
}

struct Fixture
{
    fs::path root;
    revia::runtime::TestRunConfig config;

    explicit Fixture(const fs::path& executable, const bool nestedBuild = false)
    {
        root = executable.parent_path() / "TestCatalogFixtures" /
            ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        config.enabled = true;
        config.sourceRoot = root / "source";
        config.buildRoot = nestedBuild ? config.sourceRoot / "build" : root / "build";
        fs::create_directories(config.buildRoot);
        fs::copy_file(executable, config.buildRoot / executable.filename());
        config.ctestExecutable = REVIA_CTEST_EXECUTABLE;
        config.policyFile = config.sourceRoot / "Config/Tests/quick-allowlist.json";
        config.outputRoot = root / "results";
        config.sourceIdentity = "disposable-test-catalog-fixture";
        config.environment["PATH"] = REVIA_TEST_RUNTIME_PATH;
        Write(config.sourceRoot / "CMakeLists.txt", "project(DisposableCatalogFixture)\n");
        Write(config.sourceRoot / "Tests/input.txt", "captured fixture input\n");
        Write(config.sourceRoot / "Tools/Build/probe.ps1", "authored fingerprint fixture only; never executed\n");
        Write(config.buildRoot / "CMakeCache.txt", "CMAKE_HOME_DIRECTORY:INTERNAL=" + config.sourceRoot.generic_string() + "\n");
        const auto ids = nlohmann::json::array({"Revia.VideoPublicationLane", "Revia.DrawingIntent", "Revia.VideoRequestPolicy", "ReviaVideoFrameReplyTests"});
        nlohmann::json modules = nlohmann::json::object();
        for (const auto& id : ids)
            modules[id.get<std::string>()] = {{"purpose", "Disposable pure native launcher fixture."}, {"prerequisites", {"Fresh owned fixture build."}}};
        const nlohmann::json policy = {{"schema", 1}, {"modules", modules}, {"groups", {
            {"quick", {{"approval", "reviewed-pure-native"}, {"scope", "quick"}, {"ids", ids}}},
            {"fixture_alias", {{"approval", "reviewed-pure-native"}, {"scope", "quick"}, {"ids", {"Revia.VideoPublicationLane"}}}}}}};
        Write(config.policyFile, policy.dump());
        SetCommands(executable.filename().generic_string(), "pass");
    }

    void SetCommands(const std::string& executable, const std::string& firstMode)
    {
        const auto command = (config.buildRoot / executable).generic_string();
        std::string ctest;
        for (const auto& [name, mode] : std::vector<std::pair<std::string, std::string>>{
                 {"Revia.VideoPublicationLane", firstMode}, {"Revia.DrawingIntent", "pass"},
                 {"Revia.VideoRequestPolicy", "fail"}, {"ReviaVideoFrameReplyTests", "pass"}, {"Unknown.SameBinary", "pass"}})
        {
            ctest += "add_test(\"" + name + "\" \"" + command + "\" \"--fixture\" \"" + mode + "\"";
            if (mode == "sleep")
                ctest += " \"" + (config.buildRoot / "started.marker").generic_string() + "\"";
            ctest += ")\n";
            ctest += "set_tests_properties(\"" + name + "\" PROPERTIES TIMEOUT 3)\n";
            if (mode == "skip")
                ctest += "set_tests_properties(\"" + name + "\" PROPERTIES SKIP_RETURN_CODE 77)\n";
            if (mode == "context")
                ctest += "set_tests_properties(\"" + name + "\" PROPERTIES ENVIRONMENT \"REVIA_CATALOG_CONTEXT=registered;REVIA_LOG_DIR=" +
                    (config.buildRoot / "must-not-use").generic_string() + "\")\n";
        }
        Write(config.buildRoot / "CTestTestfile.cmake", ctest);
    }
};

revia::runtime::TestRunSnapshot Wait(TestRunRuntime& runtime)
{
    const auto until = std::chrono::steady_clock::now() + 12s;
    for (;;)
    {
        const auto snapshot = runtime.Snapshot();
        if (snapshot.phase != "discovering" && snapshot.phase != "running")
            return snapshot;
        Check(std::chrono::steady_clock::now() < until, "Owned catalog worker did not finish within its fixture budget.");
        std::this_thread::sleep_for(10ms);
    }
}

void Discover(TestRunRuntime& runtime)
{
    std::string error;
    Check(runtime.Discover(error), "Discovery rejected: " + error);
    const auto snapshot = Wait(runtime);
    Check(snapshot.phase == "ready", "Discovery failed: " + snapshot.message);
}

void TestReviewedControlledContext(const fs::path& executable, const bool nestedBuild = false)
{
    Fixture fixture(executable, nestedBuild);
    fixture.SetCommands(executable.filename().generic_string(), "context");
    std::ifstream input(fixture.config.policyFile);
    auto policy = nlohmann::json::parse(input);
    input.close();
    const nlohmann::json registration = {
        {"command", {"<BUILD_ROOT>/" + executable.filename().generic_string(), "--fixture", "context"}},
        {"properties", {{"TIMEOUT", 3.0}, {"WORKING_DIRECTORY", "<BUILD_ROOT>"},
            {"ENVIRONMENT", {"REVIA_CATALOG_CONTEXT=registered", "REVIA_LOG_DIR=<BUILD_ROOT>/must-not-use"}}}}};
    policy["modules"]["Revia.VideoPublicationLane"]["execution"] = {{"approval", "reviewed-controlled"}, {"adapter", "native"},
        {"cwd", "owned-config"}, {"environment", {{"REVIA_CATALOG_CONTEXT", "registered"}}},
        {"requiredFiles", nlohmann::json::array()}, {"interpreterArguments", nlohmann::json::array()},
        {"registrationDigest", revia::audit::ContentDigest(registration.dump())}, {"holdReason", ""}, {"requiresOwnedLoopback", false}};
    Write(fixture.config.policyFile, policy.dump());
    TestRunRuntime runtime(fixture.config);
    Discover(runtime);
    if (nestedBuild)
    {
        const auto before = runtime.Snapshot().catalogIdentity;
        Write(fixture.config.sourceRoot / "Tools/Build/probe.ps1", "changed authored fingerprint fixture; never executed\n");
        Discover(runtime);
        Check(runtime.Snapshot().catalogIdentity != before, "Authored Tools/Build inputs were omitted from catalog identity.");
    }
    std::string error;
    Check(runtime.Start("quick", error), "Reviewed controlled-context selection was rejected: " + error);
    const auto result = Wait(runtime);
    Check(result.results.size() == 4 && result.results.front().outcome == "passed", "Reviewed environment/CWD was not preserved in owned staging.");
    Check(!fs::exists(fixture.config.buildRoot / "must-not-use"), "Registered ENVIRONMENT escaped the owned log destination.");
    const auto observation = result.results.front().logFile.parent_path() / "1-context" / "logs" / "context.marker";
    Check(fs::is_regular_file(observation), "The admitted child did not publish its owned-context marker.");
}

void TestIndependentCatalogAndSelection(const fs::path& executable)
{
    Fixture fixture(executable);
    TestRunRuntime runtime(fixture.config);
    Discover(runtime);
    const auto catalog = runtime.Snapshot();
    Check(catalog.catalog.size() == 5, "Distinct CTest registrations sharing a binary were lost.");
    Check(catalog.catalog.back().scope == "unknown" && catalog.catalog.back().availability != "runnable",
        "An unreviewed registration became implicitly runnable.");
    std::string error;
    Check(!runtime.Start("Unknown.SameBinary", error) && !runtime.Start(".*", error) && !runtime.Start("live", error),
        "An arbitrary name/regex/live group crossed the typed selection boundary.");
    Check(runtime.Start("quick", error), "The reviewed quick group was rejected: " + error);
    const auto result = Wait(runtime);
    Check(result.results.size() == 4, "Quick selection lost an independent registration or ran unknown work.");
    int passed = 0;
    int failed = 0;
    for (const auto& item : result.results)
    {
        passed += item.outcome == "passed";
        failed += item.outcome == "failed";
        Check(item.fresh && !item.observedAt.empty(), "A new process observation lost its fresh timestamp.");
        Check(fs::is_regular_file(item.junitFile) && fs::is_regular_file(item.logFile), "Owned result artifacts are missing.");
    }
    Check(passed == 3 && failed == 1, "A nonzero existing check became a pass or vanished.");
    Check(fs::is_regular_file(result.outputDirectory / "results.json"), "Machine-readable checkpoint is missing.");
    Check(runtime.Start("quick", error, result.outputDirectory), "Matching bounded resume was rejected: " + error);
    const auto resumed = Wait(runtime);
    Check(resumed.results.size() == 4, "Resume lost module results.");
    for (const auto& item : resumed.results)
        if (item.id != "Revia.VideoRequestPolicy")
            Check(!item.fresh && !item.reusedFrom.empty(), "Reused passing evidence was relabeled a fresh run.");
    Check(runtime.Start("fixture_alias", error), "A checked policy group alias required a C++ selector change.");
    const auto alias = Wait(runtime);
    Check(alias.results.size() == 1 && alias.results.front().id == "Revia.VideoPublicationLane" && alias.results.front().outcome == "passed",
        "A data-defined policy group did not select exactly its existing pure registration.");
    Write(fixture.config.sourceRoot / "Tests/input.txt", "changed source input\n");
    Check(runtime.Start("quick", error, result.outputDirectory), "Revalidation could not be scheduled.");
    const auto changed = Wait(runtime);
    Check(changed.phase == "failed" && changed.results.empty(), "Changed first-party source reused prior outcomes.");

    Fixture concurrent(executable);
    concurrent.SetCommands(executable.filename().generic_string(), "pause");
    TestRunRuntime changing(concurrent.config);
    Discover(changing);
    Check(changing.Start("quick", error), "Concurrent-change fixture could not start.");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (changing.Snapshot().activeTest.empty())
    {
        Check(std::chrono::steady_clock::now() < deadline, "No admitted short-pause module appeared.");
        std::this_thread::sleep_for(10ms);
    }
    Write(concurrent.config.sourceRoot / "Tests/input.txt", "changed during owned execution\n");
    const auto stale = Wait(changing);
    Check(stale.phase == "failed" && stale.results.size() == 4, "A concurrent source edit lost accounting or retained a qualified summary.");
    for (const auto& row : stale.results)
        Check(row.outcome != "passed", "A changed input context was presented as a current pass.");
}

void TestCancellationAndUnavailable(const fs::path& executable)
{
    Fixture fixture(executable);
    fixture.SetCommands(executable.filename().generic_string(), "sleep");
    TestRunRuntime runtime(fixture.config);
    Discover(runtime);
    std::string error;
    Check(runtime.Start("quick", error), "Cancellation fixture could not start.");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!fs::is_regular_file(fixture.config.buildRoot / "started.marker"))
    {
        Check(std::chrono::steady_clock::now() < deadline, "The disposable child did not publish its admission marker.");
        std::this_thread::sleep_for(10ms);
    }
    const auto cancelStarted = std::chrono::steady_clock::now();
    runtime.Cancel();
    runtime.Shutdown();
    Check(std::chrono::steady_clock::now() - cancelStarted < 2s, "Cancellation did not join the admitted disposable child within its budget.");
    const auto cancelled = runtime.Snapshot();
    Check(cancelled.phase == "cancelled", "Shutdown lost the cancellation outcome.");
    Check(cancelled.results.size() == 4 && cancelled.results.front().outcome == "cancelled", "Cancellation lost its admitted module or unstarted accounting.");
    for (std::size_t index = 1; index < cancelled.results.size(); ++index)
        Check(cancelled.results[index].outcome == "not_run" && !cancelled.results[index].fresh,
            "An unstarted module became a fresh observation or lost its not_run outcome.");

    Fixture missing(executable);
    missing.SetCommands("not-built.exe", "pass");
    TestRunRuntime unavailable(missing.config);
    Discover(unavailable);
    Check(unavailable.Start("quick", error), "Unavailable selection could not produce truthful results.");
    const auto blocked = Wait(unavailable);
    Check(blocked.results.size() == 4, "Unavailable modules disappeared from accounting.");
    for (const auto& item : blocked.results)
        Check(item.outcome == "blocked" && !item.fresh, "Missing binaries became successful observations.");

    Fixture skipped(executable);
    skipped.SetCommands(executable.filename().generic_string(), "skip");
    TestRunRuntime skipRuntime(skipped.config);
    Discover(skipRuntime);
    Check(skipRuntime.Start("quick", error), "Skip fixture could not start.");
    const auto skipResult = Wait(skipRuntime);
    Check(skipResult.results.size() == 4 && skipResult.results.front().outcome == "skipped" && skipResult.results.front().fresh,
        "CTest exit zero with SKIP_RETURN_CODE 77 was promoted to a pass or lost its executed observation.");
}
} // namespace

int main(const int argc, char** argv)
{
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--fixture")
    {
        const std::string mode = argv[2];
        if (mode == "sleep" && argc == 4)
            Write(fs::path(argv[3]), "disposable child admitted\n");
        if (mode == "sleep")
            std::this_thread::sleep_for(10s);
        if (mode == "pause")
            std::this_thread::sleep_for(500ms);
        if (mode == "context")
        {
            const auto environment = [](const char* name) { const auto* value = std::getenv(name); return value ? std::string(value) : std::string(); };
            const auto context = fs::current_path().parent_path();
            Check(environment("REVIA_CATALOG_CONTEXT") == "registered", "Registered semantic environment was lost.");
            Check(fs::is_directory(fs::current_path() / "Config") && fs::is_empty(fs::current_path() / "Config"), "Owned launch Config was not empty.");
            Check(fs::weakly_canonical(fs::path(environment("REVIA_LOG_DIR"))) == context / "logs", "Mutable registered logs overrode owned context.");
            Check(fs::weakly_canonical(fs::path(environment("TEMP"))) == context / "temp", "Owned TEMP was not applied.");
            Check(fs::weakly_canonical(fs::path(environment("USERPROFILE"))) == context / "profile", "Fresh owned profile was not applied.");
            Write(context / "logs/context.marker", "reviewed owned context admitted\n");
        }
        std::cout << "disposable existing-style module: " << mode << '\n';
        return mode == "fail" ? 7 : mode == "skip" ? 77 : 0;
    }
    try
    {
        const auto executable = fs::canonical(argv[0]);
        TestReviewedControlledContext(executable, true);
        TestReviewedControlledContext(executable);
        TestIndependentCatalogAndSelection(executable);
        TestCancellationAndUnavailable(executable);
        std::cout << "Catalog preserves independent checks, typed selection, truthful results, resume identity and owned cancellation.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Test catalog failure: " << error.what() << '\n';
        return 1;
    }
}
