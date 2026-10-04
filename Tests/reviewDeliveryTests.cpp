#include "Agents/responseFilterSettings.h"
#include "Core/profile.h"
#include "LLM/endpointSettings.h"
#include "testSupport.h"
#include "Agents/conversationAgent.h"
#include "Agents/responseFilter.h"
#include "Core/conversationContext.h"
#include "Core/utf8.h"
#include "Memory/conversationArchive.h"
#include "Visual/svgCanvas.h"

#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using revia::tests::Check;
using json = nlohmann::json;

class DeliveryBackend
{
public:
    DeliveryBackend(const std::string& answer, const std::size_t split, const int status = 200)
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"delivery-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions", [this, answer, split, status](const auto& request, auto& response)
        {
            if (status != 200)
            {
                response.status = status;
                response.set_content(answer, "text/plain");
                return;
            }
            const json body = json::parse(request.body);
            if (!body.value("stream", false))
            {
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"content", answer}}}, {"finish_reason", "stop"}}})}}.dump(), "application/json");
                return;
            }
            const auto first = answer.substr(0, split);
            const auto second = answer.substr(split);
            response.set_chunked_content_provider("text/event-stream",
                [this, first, second](const std::size_t offset, httplib::DataSink& sink)
                {
                    const json chunk = {{"choices", json::array({{
                        {"delta", {{"content", offset == 0 ? first : second}}},
                        {"finish_reason", offset == 0 ? json(nullptr) : json("stop")}}})}};
                    const auto data = "data: " + chunk.dump() + "\n\n";
                    sink.write(data.data(), data.size());
                    if (offset == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    else { generationFinished = true; sink.write("data: [DONE]\n\n", 14); sink.done(); }
                    return true;
                });
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind delivery backend.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(server.is_running(), "Delivery backend did not start.");
    }
    ~DeliveryBackend() { server.stop(); thread.join(); }
    std::atomic<bool> generationFinished = false;
    int port = 0;
private:
    httplib::Server server;
    std::jthread thread;
};

class AnswerReviewBackend
{
  public:
    AnswerReviewBackend(std::string candidate, std::string verdict)
    {
        server.Get(
            "/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models",
            [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"answer-review-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions",
            [this, candidate = std::move(candidate), verdict = std::move(verdict)](const auto& request, auto& response)
            {
                const json body = json::parse(request.body);
                {
                    std::lock_guard lock(mutex);
                    requests.push_back(body);
                }
                const bool reviewing =
                    body["messages"][0]["content"].get<std::string>().find("post-generation response reviewer") != std::string::npos;
                if (body.value("stream", false))
                {
                    const json chunk = {{"choices", json::array({{{"delta", {{"content", candidate}}}, {"finish_reason", "stop"}}})}};
                    const std::string data = "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n";
                    response.set_chunked_content_provider("text/event-stream",
                        [data](const std::size_t, httplib::DataSink& sink)
                        {
                            sink.write(data.data(), data.size());
                            sink.done();
                            return true;
                        });
                    return;
                }
                response.set_content(
                    json{{"choices",
                             json::array({{{"message", {{"content", reviewing ? verdict : candidate}}}, {"finish_reason", "stop"}}})}}
                        .dump(),
                    "application/json");
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(1); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind answer review backend.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(server.is_running(), "Answer review backend did not start.");
    }
    ~AnswerReviewBackend()
    {
        server.stop();
        thread.join();
    }
    std::vector<json> Requests() const
    {
        std::lock_guard lock(mutex);
        return requests;
    }
    int port = 0;

  private:
    mutable std::mutex mutex;
    std::vector<json> requests;
    httplib::Server server;
    std::jthread thread;
};
}

void TestOptionalAnswerReviewPreservesEvidenceAndVoice()
{
    using namespace revia::agents;
    const std::string candidate = "Kite is silver and still has three wings. Blue was apparently too mainstream.";
    for (const bool reviewEnabled : {false, true})
        for (const std::string verdict :
            {R"({"verdict":"allow","reason":"Grounded expressive answer."})",
                R"({"verdict":"replace","replacement":"Kite is silver and still has three wings. I clicked the button for you.","reason":"Repair factual delivery."})",
                "malformed review"})
        {
            AnswerReviewBackend backend(candidate, verdict);
            messageRouter router;
            llmSettings settings;
            settings.host = "127.0.0.1";
            settings.port = backend.port;
            settings.modelName = "answer-review-fixture";
            settings.bAutoStartServer = false;
            settings.bVisionEnabled = false;
            settings.bAutoMaxTokens = false;
            settings.maxTokens = 256;
            embeddingSettings embeddings;
            embeddings.bEnabled = false;
            aiProfile profile;
            profile.bMemoryEnabled = false;
            router.ApplyLLMSettings(settings, embeddings, profile);
            ResponseFilterContext facts;
            facts.answerObligation = AnswerObligationMode::CharacterFirst;
            facts.privateHistoryExcluded = true;
            facts.desktopStateKnown = true;
            responseFilterSettings filters;
            filters.bAiReviewEnabled = reviewEnabled;
            const std::string input = "Correction: the glider is silver now, not blue. What is its name and wing count?";
            const std::vector<conversationMessage> context = {{"system", "EXCLUDED_REVIEW_SYSTEM_SECRET"},
                {"memory", "EXCLUDED_REVIEW_MEMORY_SECRET"}, {"user", "The glider is Kite, blue, and has three wings."},
                {"assistant", "Kite is blue and has three wings."}, {"user", input}};
            std::string delivered;
            const auto output =
                ConversationAgent{}.Execute(router, input, context, filters, facts, {}, [&](const auto& text) { delivered += text; });
            const auto requests = backend.Requests();
            Check(output.bSuccess && delivered == output.response && output.rawResponse == candidate,
                "Optional review lost raw evidence or delivered text before final approval.");
            Check(requests.size() == (reviewEnabled ? 2 : 1),
                "Optional review overrode the off preference or performed more than one review.");
            if (!reviewEnabled || verdict.find("allow") != std::string::npos || verdict == "malformed review")
                Check(output.response == candidate, "An allowed expressive answer or degraded review was unnecessarily replaced.");
            else
                Check(output.bAiFilterChanged && output.bHardFilterBlocked && output.response.find("I clicked") == std::string::npos,
                    "An AI replacement bypassed the final deterministic output boundary.");
            if (reviewEnabled)
            {
                const auto& request = requests.back();
                const std::string system = request["messages"][0]["content"];
                const std::string evidence = request["messages"][1]["content"];
                Check(system.find("Answer posture is character first") != std::string::npos &&
                          system.find("Earlier private history is excluded") != std::string::npos,
                    "The review did not receive authoritative captured answer posture and exclusion.");
                Check(system.find("Kite is blue") == std::string::npos && evidence.find("Kite is blue") != std::string::npos &&
                          evidence.find("Role: assistant") != std::string::npos && evidence.find(input) != std::string::npos,
                    "Admitted dialogue entered authoritative system facts or the complete current task was lost.");
                const auto priorEvidence = evidence.substr(evidence.find("Prior conversation evidence"));
                Check(priorEvidence.find("EXCLUDED_REVIEW_SYSTEM_SECRET") == std::string::npos &&
                          priorEvidence.find("EXCLUDED_REVIEW_MEMORY_SECRET") == std::string::npos,
                    "Review evidence copied excluded arbitrary context roles.");
                Check(system.find("A sharp or negative tone is not by itself a reason to replace") != std::string::npos &&
                          system.find("Do not manufacture a full answer to replace a legitimate character choice") != std::string::npos,
                    "The richer answer review flattened personality or authored answer freedom.");
            }
        }
}

void TestReviewDeliveryFiltering()
{
    using namespace revia::agents;
    for (const std::string answer : {"I can see the window on your monitor.",
             "I clicked the button for you.",
             "The leaves are green. The branches reach over the path, and the afternoon light "
             "falls across the stones beside the gate. I clicked the button for you."})
    for (const std::size_t split : {std::size_t(1), answer.size() / 2, answer.size() - 1})
    for (const bool observationAvailable : {false, true})
    for (const bool desktopAllowed : {false, true})
    {
        DeliveryBackend backend(answer, split);
        messageRouter router;
        llmSettings settings;
        settings.host = "127.0.0.1"; settings.port = backend.port;
        settings.modelName = "delivery-fixture"; settings.bAutoStartServer = false;
        settings.bVisionEnabled = false; settings.bAutoMaxTokens = false; settings.maxTokens = 256;
        embeddingSettings embeddings; embeddings.bEnabled = false;
        aiProfile profile; profile.bMemoryEnabled = false;
        router.ApplyLLMSettings(settings, embeddings, profile);
        ResponseFilterContext context;
        context.desktopStateKnown = true;
        context.desktopPointer = desktopAllowed;
        context.screenObservationAvailable = observationAvailable;
        responseFilterSettings filters; filters.bAiReviewEnabled = false;
        std::string delivered;
        bool earlyDelivery = false;
        const std::string input = "Describe what happened.";
        const auto result = ConversationAgent{}.Execute(router, input, {{"user", input}},
            filters, context, {}, [&](const std::string& delta)
            {
                earlyDelivery = earlyDelivery || !backend.generationFinished.load();
                delivered += delta;
            });
        Check(result.bSuccess, "Delivery fixture failed: " + result.reason);
        Check(!earlyDelivery, "Unvalidated model output reached the delivery callback before completion.");
        Check(delivered == result.response, "Speech delivery differed from the fully approved answer.");
        Check(!delivered.empty(), "No approved speech callback was delivered.");
        const bool claimsScreenSight = answer.starts_with("I can see");
        const bool forbidden = claimsScreenSight ? !observationAvailable : !desktopAllowed;
        const std::string claim = claimsScreenSight
            ? "I can see the window on your monitor." : "I clicked the button for you.";
        if (forbidden)
            Check(delivered.find(claim) == std::string::npos && result.bHardFilterBlocked,
                "Forbidden capability claim reached the speech callback.");
        else
            Check(delivered == answer, "Capability-backed positive control was unnecessarily replaced.");
    }
}

void TestReviewUtf8Output()
{
    const revia::agents::ResponseFilter filter;
    for (const std::string& unit : {std::string("\xe4\xb8\xad"), std::string("\xf0\x9f\x98\x80"), std::string("e\xcc\x81")})
    {
        std::string candidate;
        for (int i = 0; i < 3000; ++i) candidate += unit;
        for (std::size_t budget = 0; budget < 12; ++budget)
        {
            const auto prefix = revia::utf8::Prefix(candidate, budget);
            Check(prefix.size() <= budget && candidate.starts_with(prefix), "UTF-8 prefix exceeded its budget or changed text.");
            Check(!json(prefix).dump().empty(), "Shared UTF-8 prefix split a code point.");
        }
        for (const int budget : {256, 257, 1024, 4096})
        {
            const auto result = filter.ApplyHard("Describe the pattern", candidate, {}, budget);
            Check(result.text.size() <= static_cast<std::size_t>(budget), "Reply exceeded its byte budget.");
            Check(!result.blocked && result.text.size() >= static_cast<std::size_t>(budget - 4),
                "Valid multilingual output was replaced or unnecessarily discarded.");
            const auto serialized = json(result.text).dump(); // strict UTF-8 validation
            Check(json::parse(serialized).get<std::string>() == result.text, "Reply did not round-trip through JSON.");
        }
    }
    for (const std::string& malformed : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
             std::string("\xf4\x90\x80\x80"), std::string("\xe4\xb8"), std::string("\x80")})
    {
        const auto result = filter.ApplyHard("Describe the pattern", "prefix " + malformed, {}, 256);
        Check(result.blocked && result.changed, "Malformed UTF-8 was accepted as a reply.");
        Check(!json(result.text).dump().empty(), "Malformed output repair was not JSON-safe.");
    }
    const auto punctuated = filter.ApplyHard("Describe the pattern", std::string(256, 'x') + ". tail", {}, 256);
    Check(punctuated.text.size() <= 256, "Sentence punctuation at the limit exceeded the byte budget.");
    revia::agents::ResponseFilterContext context;
    context.desktopStateKnown = true;
    const auto grounded = filter.ApplyHard("Describe the pattern", "I clicked the button for you.", context, 256);
    Check(grounded.blocked && grounded.text.size() <= 256, "A grounded replacement bypassed the output byte limit.");

    revia::tests::ScopedTestDirectory directory;
    revia::memory::ArchiveLimits limits;
    limits.maxContentCharacters = 256;
    revia::memory::ConversationArchive archive((directory.root / "utf8.db").string(), limits);
    std::string error;
    Check(archive.BeginSession("utf8", error), "Could not create UTF-8 archive fixture: " + error);
    std::string content;
    for (int i = 0; i < 300; ++i) content += "\xe4\xb8\xad";
    Check(archive.Record("utf8", "assistant", content, error), "Could not archive multilingual reply: " + error);
    const auto turns = archive.LoadSession("utf8");
    Check(turns.size() == 1 && !json(turns.front().content).dump().empty(), "Archive truncation broke UTF-8.");
    Check(!archive.Record("utf8", "assistant", "\xc0\xaf", error) && !error.empty(),
        "Archive accepted malformed UTF-8.");
    conversationContext history;
    history.AddMessage("user", content);
    for (int i = 0; i < 24; ++i) history.AddMessage("user", "next turn");
    const auto compressed = history.GetCompressedHistorySummary();
    Check(!compressed.empty() && !json(compressed).dump().empty(), "Context compression broke UTF-8.");

    std::string longError;
    for (int i = 0; i < 2000; ++i) longError += "\xe4\xb8\xad";
    for (const std::string& body : {longError, std::string("invalid: \xc0\xaf")})
    {
        DeliveryBackend backend(body, 0, 500);
        messageRouter router;
        llmSettings settings;
        settings.host = "127.0.0.1"; settings.port = backend.port;
        settings.modelName = "delivery-fixture"; settings.bAutoStartServer = false;
        settings.bVisionEnabled = false; settings.bAutoMaxTokens = false; settings.maxTokens = 256;
        embeddingSettings embeddings; embeddings.bEnabled = false;
        aiProfile profile; profile.bMemoryEnabled = false;
        router.ApplyLLMSettings(settings, embeddings, profile);
        const auto failed = router.RouteMessage("Describe the pattern", {{"user", "Describe the pattern"}});
        Check(!failed.bSuccess, "HTTP failure was accepted as generated output.");
        Check(!json(failed.reason).dump().empty(), "Backend error preview broke UTF-8 event serialization.");
        if (body == longError)
            Check(failed.reason.find("\xe4\xb8\xad") != std::string::npos,
                "Valid multilingual backend error was discarded as malformed.");
        else
            Check(failed.reason.find("malformed UTF-8") != std::string::npos,
                "Malformed backend bytes were not reported separately from truncation.");
    }
}

void TestReviewSvgRepresentation()
{
    using revia::visual::SvgSanitizer;
    for (const std::string markup : {
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg"><rect fill="u&#114;l(https://example.invalid/p.svg#p)"/></svg>)SVG",
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg"><rect style="fill:u&#x72;l(https://example.invalid/p.svg#p)"/></svg>)SVG",
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:h="http://www.w3.org/1999/xlink"><use h:href="https://example.invalid/p.svg#p"/></svg>)",
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg"><rect style="fill:u\72l(https://example.invalid/x)"/></svg>)SVG",
        R"(<svg xmlns="http://www.w3.org/2000/svg"><rect unknown="value"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg"><rect xml:base="https://example.invalid/"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg"><g></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg"><rect fill="#fff" fill="#000"/></svg>)"})
        Check(!SvgSanitizer::Sanitize(markup).accepted, "Unsafe or malformed SVG representation accepted: " + markup);
    for (const std::string markup : {
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg"><rect fill="u&#114;l(#p)"/></svg>)SVG",
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:h="http://www.w3.org/1999/xlink"><use h:href="#p"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg"><text style="fill:#abc;font-size:12px">A &amp; B</text></svg>)"})
    {
        const auto result = SvgSanitizer::Sanitize(markup);
        Check(result.accepted, "Safe local SVG representation rejected: " + result.reason);
        Check(SvgSanitizer::Sanitize(result.markup).accepted, "Serialized SVG was not valid on a second pass.");
    }
}

void RunReviewDeliveryTests()
{
    TestOptionalAnswerReviewPreservesEvidenceAndVoice();
    TestReviewDeliveryFiltering();
    TestReviewUtf8Output();
    TestReviewSvgRepresentation();
    std::cout << "Reviewed delivery, UTF-8, and SVG representation regressions passed.\n";
}
