#include "testSupport.h"

#include "Core/configManager.h"
#include "Core/messageRouter.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// ISSUE-REVIA-0066, investigated instead of assumed.
//
//   ReviaTests.exe --cancellation-live [port]
//
// The finding has stood open for two deliveries with the same sentence attached to it:
// cancelling a decision ends the client's wait, and whether the server lets go is not
// established. That sentence was accurate and it was also a way of not looking.
//
// What makes this hard to reason about from the client side is that three quite
// different conditions look identical from there -- the request simply does not come
// back. They are:
//
//   ongoing generation   the server is still producing tokens nobody will read. The
//                        slot is busy and will stay busy for the whole generation.
//   occupied slot        the generation stopped, but the slot has not been released for
//                        the next request. A capacity problem with no work behind it.
//   retained cache       the slot is free and still holds this conversation's prompt
//                        prefix. Not a problem at all: it is the thing that makes the
//                        next turn fast, and llama.cpp keeps it deliberately.
//
// Only the first two are defects, they have opposite remedies, and the third is a
// feature that looks like both. So this reads /slots directly at four moments and prints
// what each one says.
//
// It starts its own server on its own port and stops only that. A server on the
// configured port belongs to somebody else.

namespace
{

using revia::tests::Check;

struct SlotView
{
    bool answered = false;
    int slots = 0;
    int processing = 0;
    // What the server says it is holding for each slot. Non-zero with nothing
    // processing is a retained cache, which is the benign case.
    std::vector<int> cachedTokens;
    std::string raw;
};

// /slots, read straight off the server.
//
// Deliberately not through the runtime's own client: this is the evidence being used to
// check the runtime's behaviour, and taking it from the thing under test would make the
// check circular.
SlotView ReadSlots(const std::string& host, const int port)
{
    SlotView view;
    httplib::Client client(host, port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(3, 0);

    const httplib::Result response = client.Get("/slots");
    if (!response || response->status != 200)
    {
        // Newer llama.cpp builds gate /slots behind a flag and answer 501. That is a
        // real outcome for this investigation and not a failure of it: it means the
        // question cannot be answered on this build, which is worth printing rather
        // than papering over with a guess.
        view.raw = response ? ("HTTP " + std::to_string(response->status) + ": " +
            response->body.substr(0, 200)) : std::string("no answer from /slots");
        return view;
    }

    view.answered = true;
    view.raw = response->body.substr(0, 400);
    const nlohmann::json body = nlohmann::json::parse(response->body, nullptr, false);
    if (!body.is_array()) return view;

    view.slots = static_cast<int>(body.size());
    for (const auto& slot : body)
    {
        if (!slot.is_object()) continue;
        // Field names have moved between llama.cpp versions. Every spelling that has
        // meant "this slot is working" is checked, because reading none of them and
        // concluding "idle" is exactly the mistake this test exists to avoid.
        const bool busy = slot.value("is_processing", false) ||
            slot.value("state", 0) == 1 ||
            slot.value("is_generating", false);
        if (busy) ++view.processing;
        int cached = slot.value("n_past", 0);
        if (cached == 0) cached = slot.value("prompt_n", 0);
        if (cached == 0) cached = slot.value("n_prompt_tokens", 0);
        view.cachedTokens.push_back(cached);
    }
    return view;
}

void Print(const std::string& moment, const SlotView& view)
{
    std::cout << "  " << std::left << std::setw(34) << moment;
    if (!view.answered)
    {
        std::cout << "unreadable -- " << view.raw << "\n";
        return;
    }
    std::cout << "slots=" << view.slots << "  processing=" << view.processing
              << "  cached=[";
    for (std::size_t index = 0; index < view.cachedTokens.size(); ++index)
    {
        if (index > 0) std::cout << ',';
        std::cout << view.cachedTokens[index];
    }
    std::cout << "]\n";
}

bool ServerAnswers(const llmSettings& settings)
{
    messageRouter router;
    llmSettings probe = settings;
    probe.bAutoStartServer = false;
    router.ApplyLLMSettings(probe, embeddingSettings{}, aiProfile{});
    return router.CheckLLMHealth().bIsAvailable;
}

bool WaitForServer(const llmSettings& settings, const int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (ServerAnswers(settings)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

} // namespace

void RunCancellationLive(const int requestedPort)
{
    std::cout << "\n========== Cancellation, on an owned server ==========\n";

    appSettings configured;
    configManager config;
    Check(config.LoadSettings(configured), "Config/settings.json could not be loaded.");

    // Started from the configured settings rather than built field by field.
    //
    // A hand-built llmSettings answers a health check and then fails every real request,
    // because the fields a request needs -- the chat path, the token limit, the sampling
    // -- are not the fields a health check reads. The first run of this test measured
    // exactly that: a server that was up, two idle slots, and two requests that failed
    // in under a tenth of a second without ever reaching it.
    llmSettings main = configured.llm;
    main.host = "127.0.0.1";
    main.port = requestedPort > 0 ? requestedPort : 8098;
    main.contextSize = 4096;
    // Two, on purpose. With one slot every measurement is confounded: a second request
    // would queue whether or not the first released anything, so "the next request was
    // slow" could never distinguish an occupied slot from a busy server.
    main.parallelRequests = 2;
    main.bAutoStartServer = true;
    main.bVisionEnabled = false;
    main.startupTimeoutSeconds = 180;
    main.serverExecutable = configured.llm.serverExecutable;

    std::cout << "\nModel:  " << main.modelPath << "\n";
    std::cout << "Port:   " << main.port
              << "  (this run's own; a server on the configured port is left alone)\n";
    std::cout << "Slots:  " << main.parallelRequests << "\n";

    if (ServerAnswers(main))
    {
        std::cout << "\nNOT MEASURED\n";
        std::cout << "  reason: something is already serving on port " << main.port
                  << ". This test will not drive or stop a process it did not start.\n";
        return;
    }

    llamaCppServerProcess process;
    std::string error;
    if (!process.Start(main, error))
    {
        std::cout << "\nNOT MEASURED\n  reason: " << error << "\n";
        return;
    }
    struct Owned
    {
        llamaCppServerProcess& process;
        ~Owned() { process.Stop(); }
    } owned{process};

    if (!WaitForServer(main, main.startupTimeoutSeconds))
    {
        std::cout << "\nNOT MEASURED\n  reason: the server never became healthy.\n";
        return;
    }
    std::cout << "\nServer is up and owned by this run.\n\n";

    Print("idle, before anything", ReadSlots(main.host, main.port));

    // A generation long enough to still be running when it is cancelled. Asked through
    // the same router the session uses, with the same cancellation mechanism, because
    // the question is about that path and not about an HTTP client in general.
    messageRouter router;
    router.ApplyLLMSettings(main, embeddingSettings{}, aiProfile{});

    // RouteMessage needs a conversation to route. An empty one is refused before the
    // request is built, which the first run of this test spent two server starts
    // discovering.
    const std::string longRequest =
        "Write a long, detailed history of the printing press, in many paragraphs.";
    const std::vector<conversationMessage> conversation{
        {"user", longRequest}};

    std::stop_source source;
    std::atomic<bool> finished{false};
    std::atomic<long long> clientMs{0};
    const auto askStarted = std::chrono::steady_clock::now();

    std::thread asker([&]
    {
        // The same entry point a turn uses, with the same cancellation mechanism. The
        // question is about that path, not about an HTTP client in general.
        const responseOutput answer = router.RouteMessage(
            longRequest, conversation, source.get_token());
        clientMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - askStarted).count());
        finished.store(true);
        if (!answer.bSuccess && !answer.reason.empty())
        {
            std::cout << "  (the cancelled request reported: " << answer.reason
                      << ")\n";
        }
    });

    // Long enough that generation is genuinely under way rather than still in prompt
    // processing, which would measure a different thing.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const SlotView during = ReadSlots(main.host, main.port);
    Print("during generation", during);

    const auto cancelledAt = std::chrono::steady_clock::now();
    source.request_stop();
    asker.join();
    const long long clientReleasedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cancelledAt).count();
    std::cout << "  " << std::left << std::setw(34) << "client released after"
              << clientReleasedMs << "ms\n";

    // Immediately: is the slot free, or is the server still producing tokens for a
    // request nobody is listening to?
    const SlotView afterCancel = ReadSlots(main.host, main.port);
    Print("immediately after cancelling", afterCancel);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const SlotView settled = ReadSlots(main.host, main.port);
    Print("1.5s after cancelling", settled);

    // And the question that actually matters to a user: does the next request wait?
    const auto nextStarted = std::chrono::steady_clock::now();
    std::stop_source nextSource;
    const std::vector<conversationMessage> shortTurn{{"user", "Say the word ready."}};
    const responseOutput next = router.RouteMessage(
        "Say the word ready.", shortTurn, nextSource.get_token());
    const long long nextMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - nextStarted).count();
    std::cout << "  " << std::left << std::setw(34) << "next request took"
              << nextMs << "ms  ("
              << (next.bSuccess ? std::string("answered")
                                : "failed: " + next.reason) << ")\n";
    Print("after the next request", ReadSlots(main.host, main.port));

    // ---- what this establishes ----
    std::cout << "\n-- reading --\n";
    if (!during.answered)
    {
        std::cout << "  /slots is not readable on this build, so the server side of "
                     "ISSUE-REVIA-0066\n  remains unestablished. The client half is "
                     "covered by actionCancellationTests.\n";
        std::cout << "  Raw: " << during.raw << "\n";
        return;
    }

    Check(during.processing > 0,
        "Nothing was processing during what was supposed to be a long generation, so "
        "this run measured cancellation of a request that had already finished.");

    const bool slotFreed = settled.processing == 0;
    const bool cacheRetained = std::any_of(settled.cachedTokens.begin(),
        settled.cachedTokens.end(), [](const int cached) { return cached > 0; });

    std::cout << "  ongoing generation after cancel: "
              << (afterCancel.processing > 0 ? "yes" : "no") << "\n";
    std::cout << "  slot still occupied 1.5s later:  "
              << (slotFreed ? "no" : "YES") << "\n";
    std::cout << "  prompt cache retained:           "
              << (cacheRetained ? "yes" : "no")
              << "  (benign: it is what makes the next turn fast)\n";
    std::cout << "  next request latency:            " << nextMs << "ms\n";

    if (slotFreed)
    {
        std::cout << "\n  The slot was released. A cancelled decision does not hold "
                     "inference capacity,\n  which is the half of ISSUE-REVIA-0066 that "
                     "was never established.\n";
    }
    else
    {
        std::cout << "\n  The slot was NOT released. A cancelled decision continues to "
                     "occupy capacity,\n  which is a latency and throughput problem and "
                     "not a safety one -- nothing acts on\n  a late answer, which is "
                     "separately tested.\n";
    }

    // Deliberately not an assertion about which way it goes. The point of an
    // investigation is to find out; a test that demanded one answer would have been
    // written by someone who already knew, and this finding stayed open precisely
    // because nobody did.
    std::cout << "\n  Measured on an owned server. Nothing on the configured port was "
                 "touched.\n";
}
