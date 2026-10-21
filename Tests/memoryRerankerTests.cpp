#include "Memory/memoryReranker.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace revia::memory;
using namespace std::chrono_literals;

namespace
{
void Check(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckOrderValidation()
{
    const std::vector<std::size_t> original{0, 1, 2};
    RerankResult good{true, RerankStatus::Succeeded, {{2, 7.0}, {0, -4.0}, {1, 7.0}}};
    Check(RerankOrder(good, 3) == std::vector<std::size_t>({1, 2, 0}), "Reranker lost score order or stable ties.");
    for (const std::vector<RerankScore>& invalid : {
        std::vector<RerankScore>{{0, 1.0}, {0, 2.0}, {2, 3.0}},
        std::vector<RerankScore>{{0, 1.0}, {1, 2.0}},
        std::vector<RerankScore>{{0, 1.0}, {1, 2.0}, {3, 3.0}},
        std::vector<RerankScore>{{0, 1.0}, {1, 2.0}, {2, std::numeric_limits<double>::infinity()}},
        std::vector<RerankScore>{{0, 1.0}, {1, 2.0}, {2, std::numeric_limits<double>::quiet_NaN()}}})
    {
        good.scores = invalid;
        Check(RerankOrder(good, 3) == original, "Malformed scores changed the RRF fallback order.");
    }
    good = {false, RerankStatus::TimedOut, {{0, 1}, {1, 2}, {2, 3}}};
    Check(RerankOrder(good, 3) == original, "Failed reranking used its partial scores.");
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 5 && std::string(argv[1]) == "--live")
        {
            MemoryReranker reference;
            RerankerSettings live;
            live.enabled = true;
            live.pythonExecutable = std::filesystem::absolute(argv[2]);
            live.workerScript = std::filesystem::absolute(argv[3]);
            live.modelDirectory = std::filesystem::absolute(argv[4]);
            const auto startup = std::chrono::steady_clock::now();
            Check(reference.Start(live), "Installed CPU worker failed to start: " + reference.LastError());
            std::cout << "startup_ms=" << std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - startup).count() << '\n';
            std::vector<std::string> passages(30, "Saturn is a large planet with visible rings.");
            passages.back() = "Mars, known for its reddish appearance, is often referred to as the Red Planet.";
            for (int run = 0; run < 3; ++run)
            {
                const auto result = reference.Rerank("Which planet is known as the Red Planet?", passages);
                Check(result.succeeded && RerankOrder(result, passages.size()).front() == 29,
                      "Installed reference did not rank the relevant passage first within its deadline.");
                std::cout << "cpu_30_candidates_run" << run + 1 << "_ms=" << result.elapsedMilliseconds << '\n';
            }
            reference.Stop();
            std::cout << "Installed CPU worker and native transport reference smoke check passed.\n";
            return 0;
        }
        CheckOrderValidation();
        MemoryReranker reranker;
        const std::vector<std::string> documents{"first", "second", "third"};
        Check(reranker.Rerank("query", documents).status == RerankStatus::Unavailable, "Missing worker did not fall back.");
        std::stop_source preCancelled;
        preCancelled.request_stop();
        Check(reranker.Rerank("query", documents, preCancelled.get_token()).status == RerankStatus::Cancelled,
              "Pre-cancelled request was admitted.");
        Check(reranker.Rerank("query", std::vector<std::string>(31, "a")).status == RerankStatus::InvalidInput,
              "Overlarge shortlist was admitted.");
        if (argc != 3)
            throw std::runtime_error("Expected Python executable and fixture worker path.");
        RerankerSettings settings;
        settings.enabled = true;
        settings.pythonExecutable = std::filesystem::absolute(argv[1]);
        settings.workerScript = std::filesystem::absolute(argv[2]);
        settings.modelDirectory = settings.workerScript.parent_path();
        settings.startupTimeoutMs = 5000;
        settings.timeoutMs = 500;
        Check(reranker.Start(settings), "Owned fixture failed to start: " + reranker.LastError());
        const auto success = reranker.Rerank("normal", documents);
        Check(success.succeeded && RerankOrder(success, 3) == std::vector<std::size_t>({2, 1, 0}),
              "Authenticated native request failed.");
        for (const auto* query : {"duplicate", "incomplete", "wrong index", "nonfinite", "string score", "negative index", "oversized"})
        {
            const auto malformed = reranker.Rerank(query, documents);
            Check(!malformed.succeeded && RerankOrder(malformed, 3) == std::vector<std::size_t>({0, 1, 2}),
                  std::string("Native response did not preserve fallback: ") + query);
        }
        const auto timeoutStarted = std::chrono::steady_clock::now();
        const auto timeout = reranker.Rerank("delay", documents);
        Check(timeout.status == RerankStatus::TimedOut && !reranker.IsAvailable(), "Deadline did not stop timed-out inference.");
        Check(std::chrono::steady_clock::now() - timeoutStarted < 2500ms, "Deadline exceeded the bounded cleanup allowance.");

        Check(reranker.Start(settings), "Worker could not restart after timeout.");
        std::stop_source cancel;
        auto request = std::async(std::launch::async, [&] { return reranker.Rerank("delay", documents, cancel.get_token()); });
        std::this_thread::sleep_for(100ms);
        Check(reranker.Rerank("normal", documents).status == RerankStatus::Busy, "Concurrent rerank was queued behind inference.");
        const auto cancelStarted = std::chrono::steady_clock::now();
        cancel.request_stop();
        Check(request.get().status == RerankStatus::Cancelled, "Caller cancellation was lost.");
        Check(std::chrono::steady_clock::now() - cancelStarted < 1500ms, "Caller cancellation waited for inference.");
        Check(!reranker.IsAvailable(), "Cancelled inference retained its worker.");

        Check(reranker.Start(settings), "Worker could not restart before owner stop.");
        auto ownerRequest = std::async(std::launch::async, [&] { return reranker.Rerank("delay", documents); });
        std::this_thread::sleep_for(100ms);
        const auto stopStarted = std::chrono::steady_clock::now();
        reranker.Stop();
        Check(ownerRequest.get().status == RerankStatus::Cancelled, "Lifecycle Stop did not cancel in-flight request.");
        Check(std::chrono::steady_clock::now() - stopStarted < 1500ms && !reranker.IsAvailable(),
              "Lifecycle Stop did not promptly join worker resources.");
        std::cout << "Memory reranker validation, fallback, transport, timeout, cancellation and restart checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
