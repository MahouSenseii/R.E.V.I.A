#include "Agents/conversationStylePolicy.h"
#include "Agents/conversationAgent.h"
#include "Agents/reasoningRelay.h"
#include "LLM/LLamaCPP/llamaCppService.h"
#include "Runtime/conversationRuntime.h"
#include "Core/conversationContext.h"
#include "testSupport.h"

#include <httplib.h>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
using revia::tests::Check;
class ReplyBackend
{
  public:
    static constexpr std::size_t WordByWord = static_cast<std::size_t>(-1);

    ReplyBackend()
    {
        server.Get(
            "/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
            { response.set_content(R"({"data":[{"id":"structured-reply-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                std::lock_guard lock(mutex);
                lastRequest = nlohmann::json::parse(request.body);
                // Only the conversation reply carries stop markers; background calls stream too.
                if (!lastRequest.contains("stop"))
                {
                    response.set_content(
                        nlohmann::json{
                            {"choices", nlohmann::json::array({{{"message", {{"content", reviewAnswer}}}, {"finish_reason", "stop"}}})}}
                            .dump(),
                        "application/json");
                    return;
                }
                // One chunk unless a chunk size was asked for, the way the server streams tokens.
                // Word by word, each piece carries the space in front of it, as llama.cpp's do.
                std::string events;
                const std::size_t step = chunkSize == 0 ? std::max<std::size_t>(answer.size(), 1) : chunkSize;
                for (std::size_t at = 0; at < answer.size() || at == 0;)
                {
                    std::size_t next = at + step;
                    if (chunkSize == WordByWord)
                        next = std::min(answer.find(' ', at + 1), answer.size());
                    next = std::max(next, at + 1);
                    const bool last = next >= answer.size();
                    const nlohmann::json chunk = {{"choices",
                        nlohmann::json::array({{{"delta", {{"content", answer.substr(at, next - at)}}},
                            {"finish_reason", last ? nlohmann::json(finishReason) : nlohmann::json(nullptr)}}})}};
                    events += "data: " + chunk.dump() + "\n\n";
                    at = next;
                }
                response.set_content(events + "data: [DONE]\n\n", "text/event-stream");
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(1); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Structured reply backend could not bind loopback.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(server.is_running(), "Structured reply backend did not start.");
    }
    ~ReplyBackend()
    {
        server.stop();
        worker.join();
    }
    void Reply(std::string text, std::string finish = "stop", const std::size_t chunk = 0)
    {
        std::lock_guard lock(mutex);
        answer = std::move(text);
        finishReason = std::move(finish);
        chunkSize = chunk;
    }
    nlohmann::json Request()
    {
        std::lock_guard lock(mutex);
        return lastRequest;
    }
    void Review(std::string verdict)
    {
        std::lock_guard lock(mutex);
        reviewAnswer = std::move(verdict);
    }
    int port = 0;

  private:
    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    std::string answer;
    std::string finishReason;
    std::size_t chunkSize = 0;
    std::string reviewAnswer;
    nlohmann::json lastRequest;
};

// Serves exactly the server-sent events a test scripts, and records every request.
// Streamed requests get the scripted events; a request with stream:false gets
// `plainBody`, so a test can see which transport a call asked for.
class WireBackend
{
  public:
    WireBackend()
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
            { response.set_content(R"({"data":[{"id":"structured-reply-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                std::lock_guard lock(mutex);
                requests.push_back(nlohmann::json::parse(request.body));
                if (!requests.back().value("stream", false))
                {
                    response.set_content(plainBody, "application/json");
                    return;
                }
                std::string body;
                for (const auto& event : events)
                    body += "data: " + event.dump() + "\n\n";
                response.set_content(body + "data: [DONE]\n\n", "text/event-stream");
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(1); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Wire backend could not bind loopback.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(server.is_running(), "Wire backend did not start.");
    }
    ~WireBackend()
    {
        server.stop();
        worker.join();
    }
    // One streamed delta, as llama-server sends it.
    static nlohmann::json Delta(const std::string& field, const std::string& text, const nlohmann::json& finish = nullptr)
    {
        return {{"choices", nlohmann::json::array({{{"delta", {{field, text}}}, {"finish_reason", finish}}})}};
    }
    void Script(std::vector<nlohmann::json> scripted, std::string plain = {})
    {
        std::lock_guard lock(mutex);
        events = std::move(scripted);
        plainBody = std::move(plain);
    }
    std::vector<nlohmann::json> Requests()
    {
        std::lock_guard lock(mutex);
        return requests;
    }
    int port = 0;

  private:
    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    std::vector<nlohmann::json> events;
    std::string plainBody;
    std::vector<nlohmann::json> requests;
};

const latencySample* FindSample(const responseOutput& output, const std::string& stage)
{
    const auto found =
        std::find_if(output.timings.begin(), output.timings.end(), [&stage](const auto& sample) { return sample.stage == stage; });
    return found == output.timings.end() ? nullptr : &*found;
}

void Configure(llamaCppService& service, int port)
{
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = "structured-reply-fixture";
    settings.contextSize = 8192;
    settings.maxTokens = 512;
    settings.bAutoMaxTokens = settings.bAutoStartServer = settings.bVisionEnabled = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = embeddings.bAutoStartServer = false;
    aiProfile profile;
    profile.bMemoryEnabled = false;
    profile.systemPrompt = "You are Revia. Keep your expressive personality and never invent runtime facts.";
    profile.bHasTemperatureOverride = true;
    profile.temperature = 0.75F;
    service.ApplySettings(settings, embeddings, profile);
}

void TestProviderContractAndDataPreservation()
{
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    llamaCppService service((directory.root / "structured.db").string());
    Configure(service, backend.port);
    const std::string payload = R"({"literal":"<think>sample</think> User: a  b *smiles*","emoji":"🚀","x":1.25})";
    backend.Reply(payload);
    std::string emitted;
    const auto result = service.GenerateResponse(
        {{"user", "Return JSON exactly preserving the given data."}}, {}, [&](const auto& delta) { emitted += delta; });
    const auto request = backend.Request();
    Check(request.contains("response_format"), "An explicit JSON request did not receive a backend format contract.");
    Check(result.bSuccess && result.response == payload && emitted == payload && result.reasoning.empty(),
        "The provider changed literal JSON data or emitted a different structured reply.");
    Check(request.at("response_format").at("json_schema").at("schema").at("type") == "object",
        "JSON object format did not reach the actual completion request.");
    Check(request.at("temperature") == 0.75F && request.at("messages").at(0).at("content").get<std::string>().starts_with("You are Revia."),
        "Structured response handling changed the authored profile or temperature.");
    Check(request.at("stop").empty() && !request.contains("dry_multiplier"),
        "Conversational presentation penalties or stops constrain exact JSON data.");

    backend.Reply("[1,true,null,{\"x\":\"z\"}]");
    const auto array = service.GenerateResponse({{"user", "Return a JSON array."}}, {}, {}, true);
    Check(array.bSuccess && array.response == "[1,true,null,{\"x\":\"z\"}]" &&
              backend.Request().at("response_format").at("json_schema").at("schema").at("type") == "array",
        "A JSON array request was forced into an object.");
    Check(!backend.Request().at("chat_template_kwargs").at("enable_thinking").get<bool>(),
        "Structured output enabled inline reasoning that can invalidate its data format.");

    backend.Reply("Fine, I still think my idea is cooler.");
    const auto conversation =
        service.GenerateResponse({{"user", "Return JSON."}, {"assistant", "{}"}, {"user", "Tease me about that idea."}});
    Check(conversation.bSuccess && !backend.Request().contains("response_format") && backend.Request().at("dry_multiplier") == 0.8,
        "A previous structured turn changed the next ordinary conversational request.");
    for (const std::string input : {"Explain what JSON means.", "She said: \"Return JSON.\" What did she mean?",
             "Never, ever, return JSON.", "Do not, under any circumstances, return JSON."})
    {
        Check(service.GenerateResponse({{"user", input}}).bSuccess && !backend.Request().contains("response_format"),
            "A quoted or explanatory JSON mention forced a machine-readable answer.");
    }
}

void TestIncompleteDataNeverBecomesSuccessfulProse()
{
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    llamaCppService service((directory.root / "incomplete.db").string());
    Configure(service, backend.port);
    for (const auto& sample : std::vector<std::pair<std::string, std::string>>{{"{\"x\":\"Partial sentence. Another", "length"},
             {"{\"x\":1}", "length"}, {"{\"x\":", "stop"}, {"{\"x\":1,\"x\":2}", "stop"}, {"[1,2]", "stop"}, {"{} commentary", "stop"}})
    {
        backend.Reply(sample.first, sample.second);
        std::string emitted;
        const auto result =
            service.GenerateResponse({{"user", "Return JSON with key x."}}, {}, [&](const auto& delta) { emitted += delta; });
        Check(!result.bSuccess && !result.bShouldRemember && emitted.empty(),
            "Incomplete or wrong-shaped data was published as a successful reply.");
    }
}

void TestExplicitShapeWireAndValidation()
{
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    llamaCppService service((directory.root / "shape-wire.db").string());
    Configure(service, backend.port);
    const std::string input = R"(Return JSON with exactly keys "camelCase" and count.)";
    backend.Reply(R"({"camelCase":null,"count":"literal"})");
    Check(service.GenerateResponse({{"user", input}}).bSuccess, "Explicit untyped keys refused literal value types.");
    const auto wire = backend.Request().at("response_format").at("json_schema").at("schema");
    Check(wire.at("properties").at("camelCase").empty() && wire.at("properties").at("count").empty() &&
              wire.at("required") == nlohmann::json::array({"camelCase", "count"}) && !wire.at("additionalProperties").get<bool>(),
        "The actual completion request lost exact keys, spelling or untyped values.");
    for (const std::string payload :
        {R"({"camelCase":null})", R"({"camelcase":null,"count":1})", R"({"camelCase":null,"count":1,"extra":false})"})
    {
        backend.Reply(payload);
        std::string delivered;
        const auto result = service.GenerateResponse({{"user", input}}, {}, [&](const auto& text) { delivered += text; });
        Check(!result.bSuccess && !result.bShouldRemember && delivered.empty(),
            "A backend key violation became successful or delivered structured output.");
    }
    const std::string schemaInput =
        R"(Return JSON using this schema: {"type":"object","properties":{"counts":{"type":"array","items":{"type":"integer"},"minItems":1,"maxItems":2}},"required":["counts"],"additionalProperties":false})";
    backend.Reply(R"({"counts":[1,2]})");
    Check(service.GenerateResponse({{"user", schemaInput}}).bSuccess &&
              backend.Request().at("response_format").at("json_schema").at("schema").at("properties").at("counts").at("items").at("type") ==
                  "integer",
        "The actual request did not carry the explicit nested schema.");
    for (const std::string payload : {R"({"counts":[]})", R"({"counts":["1"]})", R"({"counts":[1,2,3]})"})
    {
        backend.Reply(payload);
        Check(!service.GenerateResponse({{"user", schemaInput}}).bSuccess, "A backend nested-schema violation passed native validation.");
    }
    backend.Reply("{}");
    Check(service.GenerateResponse({{"user", R"(Return JSON using this schema: {"type":"object","$ref":"remote"})"}}).bSuccess &&
              backend.Request().at("response_format").at("json_schema").at("schema") == nlohmann::json{{"type", "object"}},
        "Unsupported schema keywords did not retain the type-only wire contract.");
    backend.Reply("Still my favourite idea.");
    Check(service.GenerateResponse({{"user", input}, {"assistant", R"({"camelCase":null,"count":1})"}, {"user", "What do you think?"}})
                  .bSuccess &&
              !backend.Request().contains("response_format") && backend.Request().at("temperature") == 0.75F,
        "The explicit shape contract leaked into the next ordinary turn or changed the authored temperature.");
}

void TestTerminalDeliveryRetainsWireShape()
{
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    llamaCppService service((directory.root / "delivery-shape-wire.db").string());
    Configure(service, backend.port);
    const std::string keyInput =
        R"(Return JSON with exactly keys "measurementValue". Return only the JSON object, without prose or a code fence.)";
    const std::string payload = R"({"measurementValue":3.75})";
    backend.Reply(payload);
    Check(service.GenerateResponse({{"user", keyInput}}).bSuccess, "Compatible delivery refused matching explicit-key data.");
    const auto keyWire = backend.Request().at("response_format").at("json_schema").at("schema");
    Check(keyWire.at("properties").at("measurementValue").empty() &&
              keyWire.at("required") == nlohmann::json::array({"measurementValue"}) && !keyWire.at("additionalProperties").get<bool>(),
        "The actual wire contract lost explicit keys after a terminal delivery reaffirmation.");
    for (const std::string answer : {"{}", R"({"measurementvalue":3.75})", R"({"measurementValue":3.75,"extra":false})"})
    {
        backend.Reply(answer);
        std::string delivered;
        const auto invalid = service.GenerateResponse({{"user", keyInput}}, {}, [&](const auto& text) { delivered += text; });
        Check(!invalid.bSuccess && !invalid.bShouldRemember && delivered.empty(),
            "Compatible delivery silently discarded key validation or published invalid data.");
    }
    const std::string schemaInput =
        R"(Return JSON using this schema: {"type":"object","properties":{"sampleRatio":{"type":"number"}},"required":["sampleRatio"],"additionalProperties":false}. Return only JSON.)";
    const std::string fractional = R"({"sampleRatio":1.25})";
    backend.Reply(fractional);
    std::string delivered;
    const auto numeric = service.GenerateResponse({{"user", schemaInput}}, {}, [&](const auto& text) { delivered += text; });
    Check(numeric.bSuccess && numeric.response == fractional && delivered == fractional &&
              backend.Request().at("response_format").at("json_schema").at("schema").at("properties").at("sampleRatio").at("type") ==
                  "number",
        "The actual wire contract lost or strengthened a number schema, or rewrote its fractional data.");
    backend.Reply(R"({"sampleRatio":"1.25"})");
    delivered.clear();
    Check(
        !service.GenerateResponse({{"user", schemaInput}}, {}, [&](const auto& text) { delivered += text; }).bSuccess && delivered.empty(),
        "A schema type violation passed after a terminal delivery reaffirmation.");
    backend.Reply("{}");
    Check(service.GenerateResponse({{"user", "Return JSON with keys previousField. Change the shape requirement. Return only JSON."}})
                  .bSuccess &&
              backend.Request().at("response_format").at("json_schema").at("schema") == nlohmann::json{{"type", "object"}},
        "Unknown intervening instructions resurrected old keys on the actual wire request.");
}

void TestReviewerReplacementMustMatchFinalContract()
{
    using namespace revia::agents;
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    messageRouter router((directory.root / "review-shape.db").string());
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = backend.port;
    settings.modelName = "structured-reply-fixture";
    settings.contextSize = 8192;
    settings.maxTokens = 512;
    settings.bAutoMaxTokens = settings.bAutoStartServer = settings.bVisionEnabled = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = embeddings.bAutoStartServer = false;
    aiProfile profile;
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(settings, embeddings, profile);
    Check(router.IsLLMAvailable(), "The structured reviewer fixture failed its configured-model health admission.");
    responseFilterSettings filters;
    filters.bAiReviewEnabled = true;
    const std::string input = "Return JSON with exactly keys empty and count.";
    const std::string candidate = R"({"empty":true,"count":0})";
    for (const std::string replacement : {R"({"empty":true})", R"({"Empty":true,"count":0})", R"({"empty":true,"count":0,"extra":1})"})
    {
        backend.Reply(candidate);
        backend.Review(nlohmann::json{{"verdict", "replace"}, {"replacement", replacement}, {"reason", "Fixture replacement."}}.dump());
        std::string delivered;
        const auto result =
            ConversationAgent{}.Execute(router, input, {{"user", input}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
        Check(!result.bSuccess && !result.bShouldRemember && !result.bWasStreamed && delivered.empty() && result.bAiFilterReviewed &&
                  result.bAiFilterChanged && result.rawResponse == candidate && result.response == replacement,
            "An AI reviewer field violation bypassed final structured validation or was silently repaired. Diagnostics: " +
                nlohmann::json{{"success", result.bSuccess}, {"reviewed", result.bAiFilterReviewed}, {"changed", result.bAiFilterChanged},
                    {"raw", result.rawResponse}, {"final", result.response}, {"reason", result.reason}, {"summary", result.filterSummary},
                    {"request", backend.Request()}}
                    .dump());
    }
    backend.Reply(candidate);
    backend.Review(R"({"verdict":"allow","reason":"Matching data."})");
    std::string delivered;
    const auto valid =
        ConversationAgent{}.Execute(router, input, {{"user", input}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
    Check(
        valid.bSuccess && delivered == candidate && valid.response == candidate, "An allowed exact-key reply was changed before delivery.");
    filters.bAiReviewEnabled = false;
    backend.Reply(R"({"empty":"<|im_start|>","count":0})");
    delivered.clear();
    const auto blocked =
        ConversationAgent{}.Execute(router, input, {{"user", input}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
    Check(blocked.bSuccess && blocked.bHardFilterBlocked && !delivered.empty() && delivered == blocked.response &&
              blocked.response.find("disallowed control text") != std::string::npos,
        "Final structured validation hid the hard filter's genuine refusal.");
    filters.bAiReviewEnabled = true;
    backend.Review(R"({"verdict":"allow","reason":"Preserve refusal."})");
    delivered.clear();
    const auto reviewedRefusal =
        ConversationAgent{}.Execute(router, input, {{"user", input}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
    Check(reviewedRefusal.bSuccess && reviewedRefusal.bHardFilterBlocked && delivered == reviewedRefusal.response && !delivered.empty(),
        "An allowed hard refusal lost its authority during final structured validation.");
    backend.Review(R"({"verdict":"replace","replacement":"{\"empty\":true}","reason":"Fixture replacement."})");
    delivered.clear();
    const auto replacedRefusal =
        ConversationAgent{}.Execute(router, input, {{"user", input}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
    Check(!replacedRefusal.bSuccess && replacedRefusal.bHardFilterBlocked && delivered.empty(),
        "A past blocked flag incorrectly exempted a later invalid reviewer replacement.");
    const std::string introduced =
        R"(Use this JSON Schema: {"type":"object","properties":{"readingRatio":{"type":"number"}},"required":["readingRatio"],"additionalProperties":false}. Return JSON with exactly keys "readingRatio". Return only valid JSON, without extra commentary.)";
    const std::string typedCandidate = R"({"readingRatio":6.25})";
    filters.bAiReviewEnabled = false;
    backend.Reply(typedCandidate);
    delivered.clear();
    const auto typedValid = ConversationAgent{}.Execute(
        router, introduced, {{"user", introduced}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
    Check(typedValid.bSuccess && typedValid.response == typedCandidate && delivered == typedCandidate &&
              backend.Request().at("response_format").at("json_schema").at("schema").at("properties").at("readingRatio").at("type") ==
                  "number",
        "The ConversationAgent final delivery path disagreed with the introduced typed schema on the actual wire.");
    backend.Reply(R"({"readingRatio":{}})");
    delivered.clear();
    const auto typedRejected = ConversationAgent{}.Execute(
        router, introduced, {{"user", introduced}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
    Check(!typedRejected.bSuccess && !typedRejected.bShouldRemember && delivered.empty(),
        "A typed candidate violation passed the ConversationAgent final delivery path.");
    filters.bAiReviewEnabled = true;
    backend.Reply(typedCandidate);
    backend.Review(R"({"verdict":"replace","replacement":"{\"readingRatio\":{}}","reason":"Fixture type replacement."})");
    delivered.clear();
    const auto typedReviewRejected = ConversationAgent{}.Execute(
        router, introduced, {{"user", introduced}}, filters, {}, {}, [&](const auto& text) { delivered += text; });
    Check(!typedReviewRejected.bSuccess && !typedReviewRejected.bShouldRemember && typedReviewRejected.bAiFilterChanged &&
              typedReviewRejected.rawResponse == typedCandidate && delivered.empty(),
        "A typed reviewer replacement bypassed the introduced schema's final native validation.");
}

void TestIntroducedSchemaWireAndReaffirmation()
{
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    llamaCppService service((directory.root / "introduced-schema-wire.db").string());
    Configure(service, backend.port);
    const std::string schemaText =
        R"({"type":"object","properties":{"thermalRatio":{"type":"number"},"enabledFlag":{"type":"boolean"}},"required":["thermalRatio","enabledFlag"],"additionalProperties":false})";
    const auto schema = nlohmann::json::parse(schemaText);
    const std::string payload = R"({"thermalRatio":2.75,"enabledFlag":true})";
    for (const std::string lead : {"Follow this JSON Schema: ", "Use this JSON Schema: "})
        for (const std::string tail :
            {". Return only valid JSON, without extra commentary.",
                R"(. Return JSON with exactly keys "enabledFlag", "thermalRatio". Return only valid JSON, without extra commentary or a code fence.)"})
        {
            const auto input = lead + schemaText + tail;
            backend.Reply(payload);
            std::string emitted;
            const auto valid = service.GenerateResponse({{"user", input}}, {}, [&](const auto& text) { emitted += text; });
            Check(valid.bSuccess && valid.response == payload && emitted == payload &&
                      backend.Request().at("response_format").at("json_schema").at("schema") == schema,
                "An introduced schema or compatible key reaffirmation lost its exact types on the actual wire.");
            for (const std::string invalid : {R"({"thermalRatio":{},"enabledFlag":true})", R"({"thermalRatio":"2.75","enabledFlag":true})",
                     R"({"thermalRatio":2.75})", R"({"thermalRatio":2.75,"enabledFlag":true,"extra":0})"})
            {
                backend.Reply(invalid);
                emitted.clear();
                const auto rejected = service.GenerateResponse({{"user", input}}, {}, [&](const auto& text) { emitted += text; });
                Check(!rejected.bSuccess && !rejected.bShouldRemember && emitted.empty(),
                    "An introduced-schema violation was repaired or delivered as successful data.");
            }
        }
    const std::string arrayInput =
        R"(Use this JSON Schema: {"type":"array","items":{"type":"number"},"minItems":1,"maxItems":2}. Return only valid JSON array. Return only valid JSON, without extra commentary.)";
    backend.Reply("[2.75]");
    Check(service.GenerateResponse({{"user", arrayInput}}).bSuccess &&
              backend.Request().at("response_format").at("json_schema").at("schema") ==
                  nlohmann::json{{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 1}, {"maxItems", 2}},
        "An introduced array schema lost its container or bounded item type on the wire.");
    for (const std::string invalid : {"[]", "[true]", "[1,2,3]", "{}"})
    {
        backend.Reply(invalid);
        Check(!service.GenerateResponse({{"user", arrayInput}}).bSuccess, "An introduced array-schema violation passed validation.");
    }
    backend.Reply("[2.75]");
    Check(
        service
                .GenerateResponse({{"user",
                    R"(Return JSON using this schema: {"type":"array","items":{"type":"number"},"minItems":1,"maxItems":2}. Return only valid JSON array. Return only valid JSON.)"}})
                .bSuccess &&
            backend.Request().at("response_format").at("json_schema").at("schema").at("type") == "array" &&
            backend.Request().at("response_format").at("json_schema").at("schema").at("items").at("type") == "number",
        "A legacy explicit array schema lost its root or item type after bare JSON delivery on the wire.");
    backend.Reply("[true]");
    Check(service.GenerateResponse(
                     {{"user",
                         R"(Follow this JSON Schema: {"type":"array","items":{"type":"string","pattern":".*"}}. Return only valid JSON.)"}})
                  .bSuccess &&
              backend.Request().at("response_format").at("json_schema").at("schema") == nlohmann::json{{"type", "array"}},
        "Unsupported introduced array constraints lost their known container on the actual fallback wire.");
    backend.Reply(R"({"differentField":[]})");
    Check(service.GenerateResponse({{"user", "Follow this JSON Schema: " + schemaText +
                                                 R"(. Return JSON with exactly keys "differentField". Return only valid JSON.)"}})
                  .bSuccess &&
              backend.Request().at("response_format").at("json_schema").at("schema").at("properties") ==
                  nlohmann::json{{"differentField", nlohmann::json::object()}},
        "An incompatible latest key directive retained old typed fields on the wire.");
    backend.Reply("Still an ordinary thought.");
    Check(
        service.GenerateResponse({{"user", "Use this JSON Schema: " + schemaText}, {"assistant", payload}, {"user", "What do you think?"}})
                .bSuccess &&
            !backend.Request().contains("response_format") && backend.Request().at("temperature") == 0.75F,
        "A schema introduction leaked grammar or temperature into the next ordinary turn.");
}
}

namespace
{
// An ordinary answer reaches the caller sentence by sentence while it is generated, every
// piece of it approved, and adds up to exactly the final reply. A reply the AI reviewer
// must read whole still arrives once.
void TestConversationRepliesStreamBySentence()
{
    using namespace revia::agents;
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    messageRouter router((directory.root / "stream.db").string());
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = backend.port;
    settings.modelName = "structured-reply-fixture";
    settings.contextSize = 8192;
    settings.maxTokens = 512;
    settings.bAutoMaxTokens = settings.bAutoStartServer = settings.bVisionEnabled = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = embeddings.bAutoStartServer = false;
    aiProfile profile;
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(settings, embeddings, profile);
    Check(router.IsLLMAvailable(), "The streaming fixture failed its configured-model health admission.");

    const std::string input = "How does static linking work?";
    const std::string answer =
        "Static linking copies the code a program uses into the program itself. The linker resolves every symbol "
        "while it builds the file. Nothing else has to be found when the program starts. That makes the file larger, "
        "and it makes deploying it simpler.";
    responseFilterSettings filters;
    filters.bAiReviewEnabled = false;
    backend.Reply(answer, "stop", 6);
    std::vector<std::string> deltas;
    const auto streamed =
        ConversationAgent{}.Execute(router, input, {{"user", input}}, filters, {}, {}, [&](const auto& text) { deltas.push_back(text); });
    std::string joined;
    for (const auto& delta : deltas)
        joined += delta;
    Check(streamed.bSuccess && streamed.bWasStreamed && deltas.size() > 1 &&
            deltas.front() == "Static linking copies the code a program uses into the program itself." && joined == streamed.response,
        "An ordinary reply was not released sentence by sentence as an exact prefix of the final reply. Deltas: " +
            nlohmann::json(deltas).dump() + " Final: " + streamed.response);

    filters.bAiReviewEnabled = true;
    backend.Review(R"({"verdict":"allow","reason":"Fixture review."})");
    deltas.clear();
    const auto reviewed =
        ConversationAgent{}.Execute(router, input, {{"user", input}}, filters, {}, {}, [&](const auto& text) { deltas.push_back(text); });
    Check(reviewed.bSuccess && deltas.size() == 1 && deltas.front() == reviewed.response,
        "A reply the AI reviewer had not read yet reached the caller early. Deltas: " + nlohmann::json(deltas).dump());
}

// A full stop that starts a token -- a path, a dotfile, a framework name -- is part of
// the answer, not the end of a sentence. Streamed word by word through the path the
// answer-quality gate measures, the delivered reply is exactly what the model wrote, and
// the text the stream gate released adds up to it too (code-023: "run it as./script.sh").
void TestPunctuationLedTokensKeepTheirSpace()
{
    using namespace revia::agents;
    revia::tests::ScopedTestDirectory directory;
    ReplyBackend backend;
    messageRouter router((directory.root / "router.db").string());
    conversationContext context;
    TurnCoordinator coordinator((directory.root / "agent.db").string());
    revia::speech::SpeechService speech;
    revia::runtime::AffectController affect;
    revia::emotion::EmotionRuntime emotions;
    revia::runtime::RuntimeEventBus events;
    logger log{directory.root / "Logs"};
    const auto filterSettings = []
    {
        responseFilterSettings filters;
        filters.bAiReviewEnabled = false;
        return filters;
    };
    revia::runtime::ConversationRuntime runtime{router, context, coordinator, speech, affect, emotions, events, log,
        [](auto, const auto&) {}, [](const auto&) {}, {}, {}, {}, filterSettings, {}, {}, {}, {}, {}, {},
        []
        {
            SelfInquiryLimits limits;
            limits.enabled = false;
            return limits;
        }};
    aiProfile profile;
    profile.id = "structured-reply-fixture";
    profile.systemPrompt = "You are Revia. Answer the actual question.";
    profile.bMemoryEnabled = false;
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = backend.port;
    settings.modelName = "structured-reply-fixture";
    settings.contextSize = 8192;
    settings.maxTokens = 512;
    settings.bAutoMaxTokens = settings.bAutoStartServer = settings.bVisionEnabled = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = embeddings.bAutoStartServer = false;
    router.ApplyLLMSettings(settings, embeddings, profile);
    Check(router.IsLLMAvailable(), "The punctuation fixture failed its configured-model health admission.");

    const std::pair<std::string, std::string> cases[] = {
        {"What does chmod +x script.sh do?", "It adds execute permission to script.sh, so you can run it as ./script.sh."},
        {"How do I start the build?", "Make the script executable first, then run ./build.sh from the project root."},
        {"Where do ignored files go?", "List them in the ignore file at the root of the repository; see .gitignore for the format."},
        {"Which runtime does the service need?", "The service targets version .NET 8, so install that SDK before building it."},
        {"How do I get to the sources from docs?", "From the docs folder, run cd ../src and build from there."},
    };
    for (const auto& [input, raw] : cases)
    {
        backend.Reply(raw, "stop", ReplyBackend::WordByWord);
        const auto evaluated = runtime.EvaluateTurn(input, {}, profile, true);
        Check(evaluated.succeeded && evaluated.rawText == raw && evaluated.text == raw,
            "The evaluated turn did not deliver the model's own spacing. Raw: " + evaluated.rawText +
                " Delivered: " + evaluated.text + " Reason: " + evaluated.reason);

        std::string released;
        const auto streamed = ConversationAgent{}.Execute(router, input, {{"user", input}}, filterSettings(), {}, {},
            [&](const auto& text) { released += text; });
        Check(streamed.bSuccess && streamed.response == raw && released == raw,
            "The streamed reply did not keep the model's own spacing. Released: " + released + " Final: " + streamed.response);
    }
}
}

// PERF-3: llama-server's final chunk says how much of the prompt it reused from its
// cache. Without it, whether the prompt cache works at all could only be guessed.
void TestBackendCacheTimingsReachTheTurnTimings()
{
    revia::tests::ScopedTestDirectory directory;
    WireBackend backend;
    llamaCppService service((directory.root / "timings.db").string());
    Configure(service, backend.port);
    auto last = WireBackend::Delta("content", " ready.", "stop");
    last["timings"] = {{"cache_n", 2048}, {"prompt_n", 37}, {"prompt_ms", 12.5}, {"predicted_n", 9}, {"predicted_ms", 80.25}};
    backend.Script({WireBackend::Delta("content", "The answer is"), last});
    const auto result = service.GenerateResponse({{"user", "Is the cache working?"}});
    Check(result.bSuccess && result.response == "The answer is ready.", "The timed reply did not complete: " + result.reason);
    const auto* cached = FindSample(result, "llama_cached_prompt");
    const auto* evaluated = FindSample(result, "llama_evaluated_prompt");
    const auto* promptMs = FindSample(result, "llama_prompt_eval");
    const auto* generated = FindSample(result, "llama_generated");
    const auto* generationMs = FindSample(result, "llama_generation");
    Check(cached != nullptr && cached->milliseconds == 2048 && cached->unit == "tok" && cached->bAggregate,
        "The backend's reused prompt tokens (timings.cache_n) were not recorded.");
    Check(evaluated != nullptr && evaluated->milliseconds == 37 && evaluated->unit == "tok",
        "The backend's evaluated prompt tokens (timings.prompt_n) were not recorded.");
    Check(promptMs != nullptr && promptMs->milliseconds == 12.5 && promptMs->unit.empty() && promptMs->bAggregate,
        "The backend's prompt evaluation time was not recorded as an aggregate duration.");
    Check(generated != nullptr && generated->milliseconds == 9 && generationMs != nullptr && generationMs->milliseconds == 80.25,
        "The backend's generation count and time were not recorded.");

    backend.Script({WireBackend::Delta("content", "No timings here.", "stop")});
    const auto silent = service.GenerateResponse({{"user", "And without timings?"}});
    Check(silent.bSuccess && FindSample(silent, "llama_cached_prompt") == nullptr,
        "A backend that reported no timings was given invented ones.");
}

// PERF-4: background calls used stream:false, and llama-server notices a client that
// left a non-streamed request only at its periodic disconnect check, so cancelling one
// for a user's turn kept the turn waiting. They now stream, assemble the text locally,
// and keep their response formats and validation exactly as before.
void TestBackgroundCallsStreamWithUnchangedContracts()
{
    revia::tests::ScopedTestDirectory directory;
    WireBackend backend;
    llamaCppService service((directory.root / "background.db").string());
    Configure(service, backend.port);
    const auto streamed = [&backend](const std::string& text, const std::string& finish = "stop")
    {
        // Split mid-token, the way a server streams, with usage on the final chunk.
        const std::size_t half = text.size() / 2;
        auto last = WireBackend::Delta("content", text.substr(half), finish);
        last["usage"] = {{"prompt_tokens", 40}, {"completion_tokens", 12}};
        backend.Script({WireBackend::Delta("content", text.substr(0, half)), last}, R"({"choices":[]})");
    };

    const std::string thought = R"({"steps":[{"question":"What is asked?","answer":"A sum."}],"settled":"It is a sum."})";
    streamed(thought);
    const auto inquiry = service.Deliberate(R"({"problem":"add two numbers"})");
    auto request = backend.Requests().back();
    Check(request.value("stream", false), "Self-inquiry still asked for a non-streamed completion.");
    Check(request.at("response_format").at("type") == "json_schema" &&
              request.at("response_format").at("json_schema").at("name") == "bounded_planning" &&
              request.at("response_format").at("json_schema").at("strict") == true &&
              !request.at("chat_template_kwargs").at("enable_thinking").get<bool>(),
        "Streaming the self-inquiry changed its JSON-schema response format or thinking setting.");
    Check(inquiry.bSuccess && inquiry.response == thought && inquiry.bTokensReported && inquiry.promptTokens == 40 &&
              inquiry.completionTokens == 12,
        "The streamed self-inquiry was not assembled into the same decision and usage: " + inquiry.reason);

    streamed(R"({"action":"silence","topic":"","query":"","rationale":"Nothing new.","confidence":0.4})");
    const auto plan = service.GenerateCuriosityPlan(R"({"recent":[]})", {"think", "silence"});
    request = backend.Requests().back();
    Check(request.value("stream", false) && request.at("response_format").at("json_schema").at("schema").at("properties").contains("action"),
        "Curiosity planning did not stream with its schema intact.");
    Check(plan.bSuccess && plan.response.find("\"silence\"") != std::string::npos, "The streamed curiosity plan was lost.");

    streamed(R"({"shouldRemember":true,"category":"preference","summary":"The user prefers concise answers.","subject":"speaker","reason":"Stated."})");
    const auto memory = service.EvaluateMemory("I prefer concise answers from now on.", "Understood.",
        revia::agents::ResponseProvenance::NormalGeneration);
    request = backend.Requests().back();
    Check(request.value("stream", false) && request.at("response_format").at("type") == "json_object",
        "Memory classification did not stream with its JSON-object format.");
    Check(memory.bSuccess && memory.bShouldRemember && memory.summary == "The user prefers concise answers.",
        "The streamed memory classification did not reach the same verdict: " + memory.reason);

    // Validation is unchanged: a classifier cut off before its JSON is still a completed
    // "remember nothing", and says why.
    streamed("{\"shouldRemember\":tr", "length");
    const auto truncated = service.EvaluateMemory("I prefer tea to coffee in the mornings.", "Noted.",
        revia::agents::ResponseProvenance::NormalGeneration);
    Check(truncated.bSuccess && !truncated.bShouldRemember && truncated.reason.find("exhausted its response budget") != std::string::npos,
        "A truncated streamed classification was not handled as before: " + truncated.reason);

    const auto image = directory.root / "frame.png";
    std::ofstream(image, std::ios::binary) << "not really a png";
    streamed("A code editor with a failing build.");
    const auto vision = service.AnalyzeImage(image, "What is on screen?", 128);
    request = backend.Requests().back();
    Check(request.value("stream", false) && vision.bSuccess && vision.response == "A code editor with a failing build.",
        "Vision did not stream, or its streamed description was lost: " + vision.reason);
}

// Native tool calling: the chat request had no `tools`, and the stream parser read only
// text, so the model could never ask for a tool. Tools now go on the wire, earlier steps
// are replayed in the OpenAI shape, and streamed tool_calls fragments come back whole.
void TestToolCallsStreamAndReplayOnTheWire()
{
    revia::tests::ScopedTestDirectory directory;
    WireBackend backend;
    llamaCppService service((directory.root / "tools.db").string());
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = backend.port;
    settings.modelName = "structured-reply-fixture";
    settings.contextSize = 32768;
    settings.maxTokens = 512;
    settings.bAutoMaxTokens = settings.bAutoStartServer = settings.bVisionEnabled = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = embeddings.bAutoStartServer = false;
    aiProfile profile;
    profile.bMemoryEnabled = false;
    service.ApplySettings(settings, embeddings, profile);

    revia::llm::ToolExchange exchange;
    exchange.definitions.push_back({"web_search", "Search the web.",
        R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})"});
    exchange.rounds.push_back({"Let me check.", {{"call_a", "web_search", R"({"query":"llama.cpp release"})"}},
        {{"call_a", "web_search", "[Tool result] b10453", false}}});
    const auto call = [](const nlohmann::json& fragments, const nlohmann::json& finish = nullptr)
    { return nlohmann::json{{"choices", nlohmann::json::array({{{"delta", {{"tool_calls", fragments}}}, {"finish_reason", finish}}})}}; };
    backend.Script({WireBackend::Delta("content", "Checking both. "),
        call({{{"index", 0}, {"id", "call_1"}, {"type", "function"}, {"function", {{"name", "web_search"}, {"arguments", "{\"que"}}}}}),
        call(
            {{{"index", 1}, {"id", "call_2"}, {"type", "function"}, {"function", {{"name", "web_search"}, {"arguments", "{\"query\":"}}}}}),
        call({{{"index", 0}, {"function", {{"arguments", "ry\":\"weather Paris\"}"}}}}}),
        call({{{"index", 1}, {"function", {{"arguments", "\"weather Rome\"}"}}}}}, "tool_calls")});
    std::string shown;
    const auto step = service.GenerateResponse(
        {{"user", "Weather in Paris and Rome?"}}, {}, [&](const std::string& delta) { shown += delta; }, false,
        revia::llm::PrivateMemoryAccess::ProfileSetting, {}, &exchange);
    auto request = backend.Requests().back();
    Check(request.contains("tools") && request.at("tools").size() == 1 && request.at("tools")[0].at("type") == "function" &&
              request.at("tools")[0].at("function").at("name") == "web_search" &&
              request.at("tools")[0].at("function").at("parameters").at("required")[0] == "query" && request.at("tool_choice") == "auto" &&
              request.at("parallel_tool_calls") == true,
        "The chat request did not offer the turn's tools in the OpenAI shape: " + request.dump());
    const auto& messages = request.at("messages");
    Check(messages.size() >= 3 && messages[messages.size() - 2].at("role") == "assistant" &&
              messages[messages.size() - 2].at("content") == "Let me check." &&
              messages[messages.size() - 2].at("tool_calls")[0].at("function").at("arguments") == R"({"query":"llama.cpp release"})" &&
              messages.back().at("role") == "tool" && messages.back().at("tool_call_id") == "call_a" &&
              messages.back().at("content") == "[Tool result] b10453",
        "The earlier tool step was not replayed after the prompt: " + messages.dump());
    Check(step.bSuccess && step.toolCalls.size() == 2 && step.toolCalls[0].id == "call_1" && step.toolCalls[0].name == "web_search" &&
              step.toolCalls[0].arguments == R"({"query":"weather Paris"})" && step.toolCalls[1].id == "call_2" &&
              step.toolCalls[1].arguments == R"({"query":"weather Rome"})",
        "Streamed tool_calls fragments were not assembled per index: " + nlohmann::json(step.toolCalls.size()).dump());
    Check(step.response == "Checking both." && shown == "Checking both.",
        "The text beside the tool calls was lost or changed: " + step.response);

    // On a last step the model is asked for text, and calls it makes anyway are dropped.
    exchange.allowCalls = false;
    const auto last = service.GenerateResponse(
        {{"user", "Weather in Paris and Rome?"}}, {}, {}, false, revia::llm::PrivateMemoryAccess::ProfileSetting, {}, &exchange);
    Check(backend.Requests().back().at("tool_choice") == "none" && last.toolCalls.empty() && last.response == "Checking both.",
        "A last step still allowed or returned tool calls.");

    // Without a tool exchange the request is exactly what it was.
    backend.Script({WireBackend::Delta("content", "Plain.", "stop")});
    const auto plain = service.GenerateResponse({{"user", "Hello there"}});
    request = backend.Requests().back();
    Check(plain.bSuccess && !request.contains("tools") && !request.contains("tool_choice") && plain.toolCalls.empty(),
        "A reply without tools carried tool fields.");

    // A tier whose context cannot hold the definitions refuses them before any request.
    llamaCppService small((directory.root / "small.db").string());
    Configure(small, backend.port);
    const auto before = backend.Requests().size();
    const auto refused = small.GenerateResponse(
        {{"user", "Weather in Paris?"}}, {}, {}, false, revia::llm::PrivateMemoryAccess::ProfileSetting, {}, &exchange);
    Check(refused.bToolsRejected && !refused.bSuccess && backend.Requests().size() == before,
        "An 8K tier was sent tool definitions it cannot hold, or did not report the refusal.");
}

// Native thinking: reasoning_content was ignored, and thinking shared the answer's
// max_tokens, so a long deliberation could end the turn before the answer began.
void TestNativeThinkingStreamsReasoningWithItsOwnBudget()
{
    revia::tests::ScopedTestDirectory directory;
    WireBackend backend;
    llamaCppService service((directory.root / "thinking.db").string());
    Configure(service, backend.port);
    backend.Script({WireBackend::Delta("reasoning_content", "The user wants a sum. "),
        WireBackend::Delta("reasoning_content", "Two plus two is four."), WireBackend::Delta("content", "It is "),
        WireBackend::Delta("content", "four.", "stop")});
    std::string thought;
    std::string shown;
    const auto deep = service.GenerateResponse({{"user", "What is two plus two, carefully?"}}, {},
        [&](const std::string& delta) { shown += delta; }, true, revia::llm::PrivateMemoryAccess::ProfileSetting,
        [&](const std::string& delta) { thought += delta; });
    auto request = backend.Requests().back();
    Check(deep.bSuccess && deep.response == "It is four." && shown == "It is four.",
        "Native thinking leaked into the visible reply, or the answer after it was lost: " + deep.response);
    Check(thought == "The user wants a sum. Two plus two is four." && deep.reasoning == thought,
        "The streamed reasoning_content was not delivered live and kept as the reply's reasoning.");
    // Configure: 512 answer tokens at 8,192 context; thinking adds min(3072, 8192 / 4).
    Check(request.at("chat_template_kwargs").at("enable_thinking").get<bool>() && request.at("max_tokens") == 512 + 2048,
        "Thinking did not get its own token allowance on top of the answer's: max_tokens=" + request.at("max_tokens").dump());

    const auto plain = service.GenerateResponse({{"user", "What is two plus two?"}});
    request = backend.Requests().back();
    Check(!request.at("chat_template_kwargs").at("enable_thinking").get<bool>() && request.at("max_tokens") == 512 &&
              plain.response == "It is four.",
        "A turn without thinking changed its token budget or reply.");

    // A model that writes its thinking inline is shown the same way, and as today the
    // block never reaches the reply.
    backend.Script({WireBackend::Delta("content", "<think>Check the units "), WireBackend::Delta("content", "first.</think>"),
        WireBackend::Delta("content", "Four metres.", "stop")});
    thought.clear();
    const auto inline_ = service.GenerateResponse({{"user", "How long is it, carefully?"}}, {}, {}, true,
        revia::llm::PrivateMemoryAccess::ProfileSetting, [&](const std::string& delta) { thought += delta; });
    Check(inline_.response == "Four metres." && inline_.reasoning == "Check the units first." && thought == "Check the units first.",
        "Inline <think> reasoning was not streamed and kept out of the reply.");
}

// conversation.deliberation = "native": a turn that would have run the separate
// self-inquiry call instead answers once with native thinking, and that thinking reaches
// the window as the same SelfInquiry events "Revia is thinking" already shows.
void TestNativeDeliberationReplacesTheSeparateInquiry()
{
    const auto run = [](const bool native, std::vector<revia::runtime::RuntimeEvent>& thinking, std::vector<nlohmann::json>& requests,
                         revia::runtime::SessionResult& result)
    {
        revia::tests::ScopedTestDirectory directory;
        WireBackend backend;
        const std::string paragraph = "I should check whether this is a cold start. " + std::string(220, 'x') + ".\n\n";
        backend.Script({WireBackend::Delta("reasoning_content", paragraph),
                           WireBackend::Delta("reasoning_content", "So the first run loads everything from disk."),
                           WireBackend::Delta("content", "The first run reads everything from disk. "),
                           WireBackend::Delta("content", "Later runs find it cached.", "stop")},
            R"({"choices":[{"message":{"content":"{\"steps\":[{\"question\":\"What is slow?\",\"answer\":\"The first run.\"}],\"settled\":\"Disk.\"}"},"finish_reason":"stop"}]})");
        const auto database = (directory.root / "native.db").string();
        messageRouter router(database);
        conversationContext history;
        revia::agents::TurnCoordinator coordinator(database);
        revia::speech::SpeechService speech;
        revia::runtime::AffectController affect;
        revia::emotion::EmotionRuntime emotions;
        revia::runtime::RuntimeEventBus events;
        logger log(directory.root / "Logs");
        revia::runtime::ConversationRuntime runtime(
            router, history, coordinator, speech, affect, emotions, events, log, [](const auto&, const auto&) {}, [](const auto&) {},
            [] { return revia::actions::CapabilitySettings::InternetAccess{}; },
            [] { return revia::actions::CapabilitySettings::DesktopControl{}; }, {},
            []
            {
                responseFilterSettings filters;
                filters.bAiReviewEnabled = false;
                return filters;
            },
            {}, {}, {}, {}, {}, {},
            [native]
            {
                revia::agents::SelfInquiryLimits limits;
                limits.enabled = true;
                limits.cooldownTurns = 0;
                limits.includeOrdinaryQuestions = true;
                limits.nativeDeliberation = native;
                return limits;
            });
        std::mutex eventsMutex;
        const auto subscription = events.Subscribe(
            [&](const revia::runtime::RuntimeEvent& event)
            {
                if (event.kind != revia::runtime::RuntimeEventKind::SelfInquiry)
                    return;
                std::lock_guard lock(eventsMutex);
                thinking.push_back(event);
            });
        aiProfile profile;
        profile.bMemoryEnabled = false;
        profile.systemPrompt = "You are Revia.";
        llmSettings settings;
        settings.host = "127.0.0.1";
        settings.port = backend.port;
        settings.modelName = "structured-reply-fixture";
        settings.maxTokens = 256;
        settings.bAutoMaxTokens = settings.bAutoStartServer = settings.bVisionEnabled = false;
        embeddingSettings embeddings;
        embeddings.bEnabled = embeddings.bAutoStartServer = false;
        router.ApplyLLMSettings(settings, embeddings, profile);
        result = runtime.Reply("Why does my program run so much slower the first time I start it?", profile, true, false);
        events.Unsubscribe(subscription);
        requests = backend.Requests();
    };

    std::vector<revia::runtime::RuntimeEvent> thinking;
    std::vector<nlohmann::json> requests;
    revia::runtime::SessionResult result;
    run(true, thinking, requests, result);
    Check(result.succeeded && result.text == "The first run reads everything from disk. Later runs find it cached.",
        "The natively thinking turn did not deliver its answer: " + result.reason);
    Check(requests.size() == 1 && requests.front().contains("stop") &&
              requests.front().at("chat_template_kwargs").at("enable_thinking").get<bool>(),
        "Native deliberation still ran a separate inquiry call, or answered without thinking: " + std::to_string(requests.size()) +
            " requests.");
    std::string shown;
    for (const auto& event : thinking)
    {
        Check(event.phase == "Thinking" && event.component == "Self-inquiry", "Streamed thinking was published under another name.");
        shown += event.message + "\n";
    }
    Check(thinking.size() == 2 && shown.find("cold start") != std::string::npos && shown.find("loads everything from disk") != std::string::npos,
        "The streamed reasoning did not reach the \"Revia is thinking\" view in readable pieces: " + std::to_string(thinking.size()) +
            " events.");
    Check(result.text.find("cold start") == std::string::npos && result.reasoning.find("Thought it through while answering") != std::string::npos &&
              result.reasoning.find("loads everything from disk") != std::string::npos,
        "Native reasoning leaked into the reply or was missing from the Thought process.");

    // The separate inquiry is still there for conversation.deliberation = "selfInquiry".
    thinking.clear();
    run(false, thinking, requests, result);
    Check(requests.size() == 2 && !requests.front().contains("stop") && !requests.back().at("chat_template_kwargs").at("enable_thinking").get<bool>(),
        "deliberation=selfInquiry no longer ran its inquiry and a non-thinking answer.");
}

// The thinking arrives a few characters at a time and each published piece becomes its
// own "Revia is thinking" block, so it is released in readable pieces.
void TestReasoningRelayReleasesReadablePieces()
{
    std::vector<std::string> released;
    revia::agents::ReasoningRelay relay([&](const std::string& text) { released.push_back(text); });
    const std::string first(250, 'a');
    for (const char character : first + "\n\nShort.\n\n")
        relay.Add(std::string(1, character));
    Check(released.size() == 1 && released.front() == first, "A paragraph long enough to show was not released at its break.");
    relay.Add(std::string(1300, 'b') + " tail");
    Check(released.size() == 2 && released.back().size() <= revia::agents::ReasoningRelay::MaximumCharacters &&
              released.back().starts_with("Short."),
        "Thinking with no paragraph break was held past the maximum piece size.");
    relay.Flush();
    Check(released.size() == 3 && released.back().ends_with("tail"), "Flush did not release the last of the thinking.");
    relay.Flush();
    Check(released.size() == 3 && relay.ReceivedCharacters() == 250 + 10 + 1305, "An empty flush published an empty block.");
}

// A local server that either enforces one API key, the way llama-server started with
// --api-key does, or accepts every key, the way one started without it does.
class KeyedBackend
{
  public:
    explicit KeyedBackend(std::string enforcedKey) : key(std::move(enforcedKey))
    {
        const auto authorized = [this](const httplib::Request& request, httplib::Response& response)
        {
            if (key.empty() || request.get_header_value("Authorization") == "Bearer " + key)
                return true;
            response.status = 401;
            response.set_content(R"({"error":{"code":401,"message":"Invalid API Key"}})", "application/json");
            return false;
        };
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
            { response.set_content(R"({"data":[{"id":"structured-reply-fixture"},{"id":"keyed-embedding"}]})", "application/json"); });
        for (const char* path : {"/v1/chat/completions", "/v1/embeddings"})
            server.Post(path, [authorized](const auto& request, auto& response)
                {
                    if (!authorized(request, response))
                        return;
                    response.status = 400;
                    response.set_content(R"({"error":{"code":400,"message":"missing input"}})", "application/json");
                });
        server.new_task_queue = [] { return new httplib::ThreadPool(1); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Keyed backend could not bind loopback.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(server.is_running(), "Keyed backend did not start.");
    }
    ~KeyedBackend()
    {
        server.stop();
        worker.join();
    }
    int port = 0;

  private:
    std::string key;
    httplib::Server server;
    std::jthread worker;
};

// NET-6: a healthy answer on 127.0.0.1:8080 was trusted, so any local process that took
// the port first was sent every private prompt. A server Revia starts holds this
// session's key, and the health check now proves it.
void TestOnlyTheKeyedServerReviaStartedIsTrusted()
{
    revia::tests::ScopedTestDirectory directory;
    const std::string sessionKey = "revia-session-key";
    const auto health = [&](const int port, const bool verify)
    {
        llamaCppService service((directory.root / "keyed.db").string());
        llmSettings settings;
        settings.host = "127.0.0.1";
        settings.port = port;
        settings.modelName = "structured-reply-fixture";
        settings.apiKey = sessionKey;
        settings.bVerifyServerKey = verify;
        settings.bAutoMaxTokens = settings.bAutoStartServer = settings.bVisionEnabled = false;
        embeddingSettings embeddings;
        embeddings.bEnabled = embeddings.bAutoStartServer = false;
        service.ApplySettings(settings, embeddings, aiProfile{});
        return service.CheckHealth();
    };
    KeyedBackend ours(sessionKey);
    KeyedBackend squatter("");
    KeyedBackend stranger("someone-else's-key");
    const auto trusted = health(ours.port, true);
    Check(trusted.bIsAvailable && trusted.status == systemStatus::Green,
        "The server holding this session's key was refused: " + trusted.reason);
    const auto open = health(squatter.port, true);
    Check(!open.bIsAvailable && open.status == systemStatus::Yellow && open.reason.find("accepts a wrong key") != std::string::npos,
        "A server that accepts any key was trusted with the conversation.");
    const auto foreign = health(stranger.port, true);
    Check(!foreign.bIsAvailable && foreign.reason.find("refused this session's key") != std::string::npos,
        "A server holding a different key was trusted with the conversation.");
    // A server the user runs (and every test fixture) is not launched with Revia's key.
    Check(health(squatter.port, false).bIsAvailable, "Key proof was enforced for a server Revia did not start.");

    const auto embeddingHealth = [&](const int port)
    {
        llamaCppEmbeddingService embeddings;
        embeddingSettings settings;
        settings.host = "127.0.0.1";
        settings.port = port;
        settings.modelName = "keyed-embedding";
        settings.apiKey = sessionKey;
        settings.bVerifyServerKey = true;
        embeddings.ApplySettings(settings);
        return embeddings.CheckHealth();
    };
    Check(embeddingHealth(ours.port).bIsAvailable, "The keyed embedding server Revia started was refused.");
    const auto openEmbedding = embeddingHealth(squatter.port);
    Check(!openEmbedding.bIsAvailable && openEmbedding.status == systemStatus::Yellow,
        "An embedding server that accepts any key was trusted with memory queries.");
}

void RunStructuredReplyTests()
{
    TestProviderContractAndDataPreservation();
    TestIncompleteDataNeverBecomesSuccessfulProse();
    TestExplicitShapeWireAndValidation();
    TestTerminalDeliveryRetainsWireShape();
    TestReviewerReplacementMustMatchFinalContract();
    TestIntroducedSchemaWireAndReaffirmation();
    TestConversationRepliesStreamBySentence();
    TestPunctuationLedTokensKeepTheirSpace();
    TestBackendCacheTimingsReachTheTurnTimings();
    TestBackgroundCallsStreamWithUnchangedContracts();
    TestOnlyTheKeyedServerReviaStartedIsTrusted();
    TestNativeThinkingStreamsReasoningWithItsOwnBudget();
    TestToolCallsStreamAndReplayOnTheWire();
    TestNativeDeliberationReplacesTheSeparateInquiry();
    TestReasoningRelayReleasesReadablePieces();
}
