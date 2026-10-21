#include "Memory/memoryNliWorker.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace revia::memory;
using namespace std::chrono_literals;

namespace
{
void Check(const bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct FixtureDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("revia-nli-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    FixtureDirectory() { std::filesystem::create_directory(path); }
    ~FixtureDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void Mode(const std::string& mode) const { std::ofstream(path / "fixture-mode.txt") << mode; }
    std::size_t Requests() const
    {
        std::ifstream input(path / "requests.jsonl");
        std::string line;
        std::size_t count = 0;
        while (std::getline(input, line))
            ++count;
        return count;
    }
    void AwaitRequest(const std::size_t expected) const
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (Requests() < expected && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(Requests() >= expected, "Owned NLI fixture did not receive the in-flight request.");
    }
};

void CheckFailure(const NliPairResult& result, const std::string& label)
{
    Check(!result.succeeded && result.modelIdentity.empty() && result.forwardEntailment == 0 &&
              result.reverseEntailment == 0 && result.forwardContradiction == 0 && result.reverseContradiction == 0,
          "Malformed or failed evidence escaped: " + label);
}
}

int main(int argc, char** argv)
{
    try
    {
        static_assert(std::is_base_of_v<SemanticDuplicateScorer, MemoryNliWorker>);
        Check(argc == 3, "Expected Python executable and NLI fixture worker path.");
        FixtureDirectory directory;
        MemoryNliWorker worker;
        CheckFailure(worker.Compare("left", "right"), "not started");
        Check(!worker.Start({}) && !worker.IsAvailable(), "Disabled scorer started a worker.");
        MemoryNliSettings settings;
        settings.enabled = true;
        settings.pythonExecutable = std::filesystem::absolute(argv[1]);
        settings.workerScript = std::filesystem::absolute(argv[2]);
        settings.modelDirectory = directory.path;
        settings.startupTimeoutMs = 5000;
        settings.timeoutMs = 500;
        auto missing = settings;
        missing.modelDirectory /= "absent";
        Check(!worker.Start(missing) && !worker.IsAvailable(), "Missing model directory was admitted.");
        auto invalidSettings = settings;
        invalidSettings.cpuThreads = 9;
        Check(!worker.Start(invalidSettings), "Unbounded CPU configuration was admitted.");
        std::stop_source preCancelled;
        preCancelled.request_stop();
        Check(!worker.Start(settings, preCancelled.get_token()), "Cancelled startup was admitted.");

        for (const auto* mode : {"wrapped port", "float port", "boolean port", "wrong model", "gpu", "token"})
        {
            directory.Mode(mode);
            Check(!worker.Start(settings) && !worker.IsAvailable(), std::string("Invalid startup receipt admitted: ") + mode);
        }
        directory.Mode("normal");
        Check(worker.Start(settings), "NLI fixture failed to start: " + worker.LastError());
        const auto result = worker.Compare("normal", "The same synthetic fact.");
        Check(result.succeeded && !result.truncated && result.modelIdentity == MemoryNliModelIdentity &&
                  result.forwardEntailment == 0.99 && result.reverseEntailment == 0.98 &&
                  result.forwardContradiction == 0.002 && result.reverseContradiction == 0.003,
              "Authenticated evidence lost a direction, label or pinned model identity.");
        for (const auto* mode : {"wrong model", "truncated", "truncated type", "incomplete", "extra", "direction type",
                 "string probability", "boolean probability", "negative", "greater than one", "probability sum",
                 "nonfinite", "infinity", "oversized", "unavailable", "duplicate key", "duplicate probability"})
            CheckFailure(worker.Compare(mode, "right"), mode);
        Check(worker.Compare("normal", "right").succeeded, "Rejected evidence corrupted the next healthy request.");
        const auto beforeInvalid = directory.Requests();
        for (const auto& invalid : {std::string{}, std::string(1025, 'x'), std::string("x\0y", 3), std::string("\xff", 1)})
        {
            CheckFailure(worker.Compare(invalid, "right"), "invalid left");
            CheckFailure(worker.Compare("left", invalid), "invalid right");
        }
        CheckFailure(worker.Compare("left", "right", preCancelled.get_token()), "pre-cancelled comparison");
        Check(directory.Requests() == beforeInvalid, "Invalid or cancelled input reached the worker.");
        const auto timeoutStarted = std::chrono::steady_clock::now();
        CheckFailure(worker.Compare("delay", "right"), "deadline");
        Check(!worker.IsAvailable() && std::chrono::steady_clock::now() - timeoutStarted < 2500ms,
              "Deadline did not stop and join owned inference promptly.");

        settings.timeoutMs = 2000;
        Check(worker.Start(settings), "NLI worker could not restart after deadline.");
        std::stop_source cancel;
        const auto beforeCancel = directory.Requests();
        auto pending = std::async(std::launch::async, [&] { return worker.Compare("delay", "right", cancel.get_token()); });
        directory.AwaitRequest(beforeCancel + 1);
        const auto busyStarted = std::chrono::steady_clock::now();
        CheckFailure(worker.Compare("normal", "right"), "busy");
        Check(std::chrono::steady_clock::now() - busyStarted < 250ms, "Busy scoring queued behind existing inference.");
        const auto cancelStarted = std::chrono::steady_clock::now();
        cancel.request_stop();
        CheckFailure(pending.get(), "caller cancellation");
        Check(!worker.IsAvailable() && std::chrono::steady_clock::now() - cancelStarted < 1500ms,
              "Cancellation did not stop owned inference promptly.");

        Check(worker.Start(settings), "NLI worker could not restart after cancellation.");
        const auto beforeStop = directory.Requests();
        auto stopping = std::async(std::launch::async, [&] { return worker.Compare("delay", "right"); });
        directory.AwaitRequest(beforeStop + 1);
        const auto stopStarted = std::chrono::steady_clock::now();
        worker.Stop();
        CheckFailure(stopping.get(), "owner stop");
        Check(!worker.IsAvailable() && std::chrono::steady_clock::now() - stopStarted < 1500ms,
              "Owner Stop did not cancel and join inference promptly.");
        Check(worker.Start(settings) && worker.Compare("normal", "right").succeeded,
              "Fresh lifecycle failed after owner Stop.");
        worker.Stop();
        worker.Stop();

        directory.Mode("delay");
        auto shortStartup = settings;
        shortStartup.startupTimeoutMs = 100;
        const auto startupStarted = std::chrono::steady_clock::now();
        Check(!worker.Start(shortStartup) && !worker.IsAvailable() &&
                  std::chrono::steady_clock::now() - startupStarted < 1500ms,
              "Startup deadline did not clean up its owned process.");
        std::stop_source startupCancel;
        auto starting = std::async(std::launch::async, [&] { return worker.Start(settings, startupCancel.get_token()); });
        std::this_thread::sleep_for(100ms);
        startupCancel.request_stop();
        Check(!starting.get() && !worker.IsAvailable(), "Startup ignored caller cancellation.");
        directory.Mode("normal");
        Check(worker.Start(settings) && worker.Compare("normal", "right").succeeded, "Startup cancellation poisoned restart.");
        worker.Stop();

        std::ifstream audit(directory.path / "requests.jsonl");
        for (std::string line; std::getline(audit, line);)
        {
            const auto sent = nlohmann::json::parse(line);
            Check(sent.is_object() && sent.size() == 2 && sent.at("left").is_string() && sent.at("right").is_string(),
                  "The scorer transported persistence authority beyond the admitted text pair.");
        }
        std::cout << "NLI native receipt, authenticated bidirectional evidence, validation, input bounds, timeout, cancellation and restart checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
