#include "Runtime/testRunRuntime.h"
#include "Actions/actionTypes.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
volatile std::sig_atomic_t interrupted = 0;
void Interrupt(int) { interrupted = 1; }

void Help()
{
    std::cout << "Existing CTest modules in reviewed owned contexts; no live workloads or dependency installation.\n"
        "ReviaTestLauncher list|run|resume --source-root <source> --build-root <build> --ctest <absolute-tool>\n"
        "  --source-id <declared-revision> --output <owned-result-root> [--configuration Debug|Release]\n"
        "  [--policy <source/Config/Tests/quick-allowlist.json>] [--runtime-path <explicit-minimal-PATH>]\n"
        "  run: [--group <approved-policy-group>]   resume: --from <prior-run-directory> [--group <approved-policy-group>]\n"
        "  list: [--json]   status|result: --output <owned-result-root> --from <prior-run-directory>\n"
        "quick = four reviewed pure modules; response = synthetic timestamp logic, no real latency measurement.\n"
        "Checked policy lists approved conversation/memory/tools/media/development/worker/Qt groups and all-reviewed.\n"
        "Select a group explicitly. Per-module safety holds, missing helpers/packages and unreviewed registration drift stay visible.\n"
        "Staged CTest preserves discovered argv and pinned semantics, with reviewed owned temp/log/profile/CWD controls.\n"
        "Results qualify this controlled context. The launcher does not build missing targets or install dependencies.\n"
        "Manual measurement tools remain separate, unmeasured and held: contextBenchmark.py starts a GPU llama server;\n"
        "Benchmark-ReviaTTS.ps1 starts Qwen and uses RuntimeData. Neither is invoked here. Real model/workload, hardware,\n"
        "configuration, sample counts and measurement units are unverified until separately approved and observed.\n"
        "Use separate output roots for concurrent launcher instances. Historical reuse never counts as a fresh pass.\n";
}

std::string Required(const std::map<std::string, std::string>& options, const std::string& key)
{
    const auto found = options.find(key);
    if (found == options.end() || found->second.empty()) throw std::runtime_error("Missing --" + key + ". See --help.");
    return found->second;
}

revia::runtime::TestRunSnapshot Wait(revia::runtime::TestRunRuntime& runtime)
{
    for (;;)
    {
        if (interrupted) runtime.Cancel();
        const auto snapshot = runtime.Snapshot();
        if (snapshot.phase != "discovering" && snapshot.phase != "running") return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void PrintSaved(const std::map<std::string, std::string>& options, const bool details)
{
    const auto bytes = revia::runtime::ReadTestRunCheckpoint(revia::actions::Utf8ToPath(Required(options, "output")),
        revia::actions::Utf8ToPath(Required(options, "from")));
    const auto document = Json::parse(bytes);
    if (document.value("schema", 0) != 1 && document.value("schema", 0) != 2) throw std::runtime_error("Unknown saved result schema.");
    if (details) std::cout << document.dump(2) << '\n';
    else
    {
        std::cout << "Recorded checkpoint: " << document.value("phase", "unknown") << "; execution liveness unverified.\n";
        for (const auto& row : document.at("results"))
            std::cout << row.at("id").get<std::string>() << ": " << row.at("outcome").get<std::string>() << " (saved observation; no new execution)\n";
    }
}
} // namespace

int main(const int argc, char** argv)
{
    try
    {
        if (argc < 2 || std::string(argv[1]) == "--help") { Help(); return 0; }
        const std::string command = argv[1];
        std::map<std::string, std::string> options;
        bool json = false;
        for (int index = 2; index < argc; ++index)
        {
            const std::string option = argv[index];
            if (option == "--json") { json = true; continue; }
            if (!option.starts_with("--") || index + 1 == argc) throw std::runtime_error("Invalid launcher option. See --help.");
            static const std::set<std::string> allowed = {"source-root", "build-root", "ctest", "source-id", "output", "configuration", "policy", "runtime-path", "group", "from"};
            const auto key = option.substr(2);
            if (!allowed.contains(key) || !options.emplace(key, argv[++index]).second) throw std::runtime_error("Unknown or repeated launcher option.");
        }
        if (command == "status" || command == "result") { PrintSaved(options, command == "result"); return 0; }
        if (command != "list" && command != "run" && command != "resume") throw std::runtime_error("Unknown launcher command. See --help.");
        revia::runtime::TestRunConfig config;
        config.enabled = true;
        config.sourceRoot = revia::actions::Utf8ToPath(Required(options, "source-root"));
        config.buildRoot = revia::actions::Utf8ToPath(Required(options, "build-root"));
        config.ctestExecutable = revia::actions::Utf8ToPath(Required(options, "ctest"));
        config.outputRoot = revia::actions::Utf8ToPath(Required(options, "output"));
        config.sourceIdentity = Required(options, "source-id");
        config.policyFile = options.contains("policy") ? revia::actions::Utf8ToPath(options.at("policy")) : config.sourceRoot / "Config/Tests/quick-allowlist.json";
        if (options.contains("configuration")) config.configuration = options.at("configuration");
        if (options.contains("runtime-path")) config.environment["PATH"] = options.at("runtime-path");
        config.environment["LANG"] = "C";
        config.environment["LC_ALL"] = "C";
        config.environment["TZ"] = "UTC";
        revia::runtime::TestRunRuntime runtime;
        std::string error;
        if (!runtime.Configure(config, error) || !runtime.Discover(error)) throw std::runtime_error(error);
        std::signal(SIGINT, Interrupt);
        auto snapshot = Wait(runtime);
        if (snapshot.phase != "ready") throw std::runtime_error(snapshot.message);
        if (command == "list")
        {
            Json entries = Json::array();
            for (const auto& entry : snapshot.catalog)
                if (json) entries.push_back({{"id", entry.id}, {"purpose", entry.purpose}, {"group", entry.group}, {"groups", entry.groups}, {"scope", entry.scope},
                    {"prerequisites", entry.prerequisites}, {"labels", entry.labels}, {"timeoutSeconds", entry.timeoutSeconds},
                    {"availability", entry.availability}, {"latestOutcome", entry.latestOutcome}, {"applicability", entry.applicability}});
                else std::cout << entry.id << " [" << entry.group << "/" << entry.scope << "] " << entry.purpose << '\n'
                    << "  " << entry.availability << "; latest=" << entry.latestOutcome << "; " << entry.applicability << '\n';
            if (json) std::cout << Json({{"catalogIdentity", snapshot.catalogIdentity}, {"modules", entries}}).dump(2) << '\n';
            return 0;
        }
        const auto group = options.contains("group") ? options.at("group") : "quick";
        const auto resume = command == "resume" ? revia::actions::Utf8ToPath(Required(options, "from")) : fs::path{};
        if (!runtime.Start(group, error, resume)) throw std::runtime_error(error);
        snapshot = Wait(runtime);
        std::cout << snapshot.phase << ": " << snapshot.message << '\n';
        for (const auto& row : snapshot.results) std::cout << row.id << ": " << row.outcome << (row.fresh ? " (new observation)" : " (no fresh execution)") << '\n';
        if (!snapshot.outputDirectory.empty()) std::cout << "Results: " << revia::actions::PathToUtf8(snapshot.outputDirectory / "results.json") << '\n';
        if (snapshot.phase == "cancelled") return 3;
        if (snapshot.phase != "completed") return 2;
        for (const auto& row : snapshot.results) if (row.outcome != "passed") return 1;
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "Test launcher: " << error.what() << '\n'; return 2; }
}
