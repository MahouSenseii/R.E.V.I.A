#include "testSupport.h"

#include "Core/messageRouter.h"
#include "LLM/llmService.h"
#include "LLM/providerCapabilities.h"
#include "Library/structLibrary.h"

#include <chrono>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

// One chat client, several kinds of server.
//
// Ollama, LM Studio, vLLM and the hosted APIs all speak OpenAI's chat completions, and
// the llama.cpp client already spoke it -- with llama.cpp's extras in every request:
// cache_prompt, chat_template_kwargs, the DRY sampler, /health, /props and /tokenize.
// A server without them answered 400 or 404, so "backend": "Ollama" in settings had
// never worked. The capabilities profile is what the client now checks before it
// relies on an extra, and the privacy rule is what keeps her private context off a
// remote brain unless the owner says so.
namespace
{
using namespace std::chrono_literals;
using revia::tests::Check;
using json = nlohmann::json;

// A server that speaks only the OpenAI chat API, or llama.cpp's superset of it.
class ChatServer
{
public:
    ChatServer(const bool llamaCpp, std::vector<std::string> models, std::string answer)
        : reply(std::move(answer))
    {
        if (llamaCpp)
        {
            server.Get("/health", [](const auto&, auto& response)
            { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
            server.Get("/props", [](const auto&, auto& response)
            {
                response.set_content(
                    R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})",
                    "application/json");
            });
        }
        server.Get("/v1/models", [models](const auto&, auto& response)
        {
            json data = json::array();
            for (const std::string& model : models) data.push_back({{"id", model}});
            response.set_content(json{{"data", data}}.dump(), "application/json");
        });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            const json body = json::parse(request.body);
            std::string answer;
            {
                std::lock_guard lock(mutex);
                ++requests;
                lastRequest = body;
                lastAuthorization = request.get_header_value("Authorization");
                answer = reply;
            }
            if (body.value("stream", false))
            {
                const json chunk = {{"choices", json::array({{
                    {"delta", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                response.set_content(
                    "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
            }
            else
            {
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"role", "assistant"}, {"content", answer}}},
                    {"finish_reason", "stop"}}})}}.dump(), "application/json");
            }
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the chat fixture server.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The chat fixture server did not start.");
    }
    ~ChatServer() { server.stop(); thread.join(); }
    int Requests() { std::lock_guard lock(mutex); return requests; }
    json LastRequest() { std::lock_guard lock(mutex); return lastRequest; }
    std::string LastAuthorization() { std::lock_guard lock(mutex); return lastAuthorization; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    std::string reply;
    int requests = 0;
    json lastRequest;
    std::string lastAuthorization;
    std::jthread thread;
};

// A JSON verdict the memory classifier accepts, so a fixture can serve both a chat
// turn (streamed, the text is spoken) and a memory evaluation (not streamed, parsed).
const char* const MemoryVerdict =
    R"({"shouldRemember":true,"category":"preference",)"
    R"("summary":"The user likes cats.","reason":"A stable preference."})";

llmSettings Settings(const std::string& backend, const int port, const std::string& model)
{
    llmSettings settings;
    settings.backend = backend;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = model;
    settings.bAutoStartServer = false;
    settings.bVisionEnabled = false;
    return settings;
}

embeddingSettings NoEmbeddings()
{
    embeddingSettings embeddings;
    embeddings.bEnabled = false;
    embeddings.bAutoStartServer = false;
    return embeddings;
}

// A port nothing listens on: bound once so it is free, then released.
int ClosedPort()
{
    httplib::Server probe;
    const int port = probe.bind_to_any_port("127.0.0.1");
    Check(port > 0, "Could not find a closed port.");
    return port;
}

void TestCapabilitiesFollowTheBackendName()
{
    using revia::llm::CapabilitiesFor;
    using revia::llm::ProviderKind;

    const auto llama = CapabilitiesFor("LLamaCpp");
    Check(llama.kind == ProviderKind::LlamaCpp && llama.healthEndpoint && llama.propsEndpoint &&
        llama.tokenizeEndpoint && llama.drySampling && llama.chatTemplateKwargs &&
        llama.cachePrompt && llama.jsonSchema && llama.strictModelList,
        "llama.cpp lost one of its own extras.");

    const auto ollama = CapabilitiesFor("Ollama");
    Check(ollama.kind == ProviderKind::Ollama && !ollama.healthEndpoint &&
        !ollama.propsEndpoint && !ollama.tokenizeEndpoint && !ollama.drySampling &&
        !ollama.chatTemplateKwargs && !ollama.cachePrompt && ollama.jsonSchema &&
        ollama.strictModelList && std::string(ollama.displayName) == "Ollama",
        "Ollama was expected to have a llama.cpp extra it does not serve.");
    Check(CapabilitiesFor("lm studio").kind == ProviderKind::LMStudio &&
        CapabilitiesFor("LMStudio").strictModelList,
        "LM Studio was not recognised by either spelling.");

    const auto hosted = CapabilitiesFor("OpenAI");
    Check(hosted.kind == ProviderKind::OpenAICompatible && !hosted.strictModelList &&
        !hosted.drySampling && !hosted.cachePrompt,
        "A hosted API was expected to be checked against its model list, which a "
        "gateway may not have.");
    Check(CapabilitiesFor("CustomHttp").kind == ProviderKind::OpenAICompatible &&
        CapabilitiesFor("something-new").kind == ProviderKind::OpenAICompatible,
        "An unknown backend name did not get the plainest profile.");

    Check(revia::llm::IsChatBackend("LLamaCpp") && revia::llm::IsChatBackend("ollama") &&
        revia::llm::IsChatBackend("OpenAI") && revia::llm::IsChatBackend("CustomHttp") &&
        !revia::llm::IsChatBackend("Placeholder") && !revia::llm::IsChatBackend(""),
        "The chat backend names are not the ones settings may use.");

    Check(revia::llm::IsLoopbackHost("127.0.0.1") && revia::llm::IsLoopbackHost("localhost") &&
        revia::llm::IsLoopbackHost("LOCALHOST") && revia::llm::IsLoopbackHost("::1") &&
        revia::llm::IsLoopbackHost("127.0.1.1") && !revia::llm::IsLoopbackHost("192.168.1.20") &&
        !revia::llm::IsLoopbackHost("api.openai.com"),
        "A host was not told apart as this machine or another.");

    llmSettings local = Settings("Ollama", 11434, "qwen3");
    Check(!revia::llm::IsCloudEndpoint(local), "A loopback Ollama was called remote.");
    local.bTreatAsRemote = true;
    Check(revia::llm::IsCloudEndpoint(local),
        "treatAsRemote did not make a loopback tunnel count as leaving the machine.");
    Check(revia::llm::IsCloudEndpoint(Settings("OpenAI", 443, "gpt")) == false &&
        revia::llm::IsCloudEndpoint([]
        {
            llmSettings hosted = Settings("OpenAI", 443, "gpt");
            hosted.host = "api.openai.com";
            return hosted;
        }()),
        "A hosted API's host was not what decided that requests leave the machine.");
}

// Pointed at a server with only the OpenAI endpoints, the client neither probes what
// is not there nor sends what would be rejected, and the same server is refused by
// the llama.cpp profile because /health is what that profile trusts.
void TestAnOpenAiCompatibleServerIsSpokenToInItsOwnDialect()
{
    ChatServer server(false, {"fixture-ollama"}, MemoryVerdict);
    llmSettings settings = Settings("Ollama", server.port, "fixture-ollama");
    settings.apiKey = "sk-fixture";
    llmService service;
    service.ApplySettings(settings, NoEmbeddings(), aiProfile{});

    const healthOutput health = service.CheckBackendHealth();
    Check(health.bIsAvailable && health.name == "Ollama" &&
        health.message.find("Ollama is online") != std::string::npos,
        "A server with only the OpenAI endpoints was not found healthy: " + health.reason);
    Check(service.IsBackendAvailable(), "The chat client did not count Ollama as available.");

    const std::vector<conversationMessage> context{{"user", "Do you like cats?"}};
    const responseOutput reply = service.GenerateResponse(context);
    Check(reply.bSuccess, "A chat turn on the Ollama profile failed: " + reply.reason);
    json request = server.LastRequest();
    Check(request.value("model", "") == "fixture-ollama" &&
        !request.contains("cache_prompt") && !request.contains("chat_template_kwargs") &&
        !request.contains("dry_multiplier") && !request.contains("dry_base"),
        "A chat request carried a llama.cpp extra to a server that rejects it: " +
        request.dump());
    Check(server.LastAuthorization() == "Bearer sk-fixture",
        "The API key was not sent as a bearer token.");

    const memoryDecision decision = service.EvaluateMemory("I like cats.", "Noted.",
        revia::agents::ResponseProvenance::NormalGeneration);
    Check(decision.bSuccess && decision.bShouldRemember,
        "Memory evaluation on the Ollama profile failed: " + decision.reason);
    request = server.LastRequest();
    Check(!request.contains("dry_multiplier") && !request.contains("chat_template_kwargs") &&
        !request.contains("top_k") && !request.contains("min_p") &&
        request.value("stream", true) == false &&
        request["response_format"].value("type", "") == "json_object",
        "The memory classifier sent llama.cpp samplers to another server: " + request.dump());

    // The same server, expected to be llama.cpp: no /health, so not llama.cpp.
    llmService strict;
    strict.ApplySettings(Settings("LLamaCpp", server.port, "fixture-ollama"),
        NoEmbeddings(), aiProfile{});
    const healthOutput notLlama = strict.CheckBackendHealth();
    Check(!notLlama.bIsAvailable && notLlama.name == "llama.cpp",
        "A server without /health passed for llama.cpp.");

    // Ollama lists what it has pulled, so a model it does not list is not there.
    llmService missing;
    missing.ApplySettings(Settings("Ollama", server.port, "not-pulled"), NoEmbeddings(),
        aiProfile{});
    const healthOutput notPulled = missing.CheckBackendHealth();
    Check(!notPulled.bIsAvailable && notPulled.reason.find("pull") != std::string::npos,
        "A model Ollama has not pulled was not reported as such: " + notPulled.reason);

    // A gateway routes to models it may not list; the name in settings is what goes
    // in the request.
    llmService gateway;
    gateway.ApplySettings(Settings("OpenAI", server.port, "any-routed-model"), NoEmbeddings(),
        aiProfile{});
    Check(gateway.IsBackendAvailable(),
        "A hosted API was refused because the model was not in its list.");
}

// A remote Fast is not handed the conversation unless the owner opts in; with Main
// local and the opt-in off, Main answers instead and says why. Memory evaluation,
// which is the exchange itself, follows the same rule.
void TestARemoteTierIsNotHandedPrivateContext()
{
    using revia::intelligence::IntelligenceTier;
    ChatServer main(true, {"fixture-main"}, "Understood.");
    ChatServer fast(false, {"fixture-fast"}, MemoryVerdict);
    const llmSettings mainSettings = Settings("LLamaCpp", main.port, "fixture-main");
    llmSettings fastSettings = Settings("Ollama", fast.port, "fixture-fast");
    // Loopback, because the fixture is, but declared to leave the machine: a tunnel.
    fastSettings.bTreatAsRemote = true;

    messageRouter router;
    router.ApplyLLMSettings(mainSettings, fastSettings, llmSettings{}, NoEmbeddings(),
        aiProfile{}, true, false);
    Check(router.TierIsRemote(IntelligenceTier::Fast) && !router.TierIsRemote(IntelligenceTier::Main),
        "The router did not tell a remote tier from a local one.");

    revia::intelligence::IntelligenceDecision decision;
    decision.requestedTier = decision.selectedTier = IntelligenceTier::Fast;
    decision.mode = revia::intelligence::ReasoningMode::Fast;
    decision.reason = "A simple social turn.";
    const std::vector<conversationMessage> context{{"user", "Do you like cats?"}};

    responseOutput routed = router.RouteMessage("Do you like cats?", context, {}, {}, decision,
        revia::llm::PrivateMemoryAccess::ProfileSetting);
    Check(routed.bSuccess, "Main did not answer the turn: " + routed.reason);
    Check(routed.selectedTier == "Main" && routed.bRoutingFallback &&
        routed.routingFallbackReason.find("remote") != std::string::npos &&
        routed.selectedModel == "fixture-main" && fast.Requests() == 0,
        "A remote Fast brain was handed the conversation without the opt-in: " +
        routed.selectedTier + " / " + routed.routingFallbackReason);

    router.SetRemotePrivacy(true);
    routed = router.RouteMessage("Do you like cats?", context, {}, {}, decision,
        revia::llm::PrivateMemoryAccess::ProfileSetting);
    Check(routed.bSuccess && routed.selectedTier == "Fast" && !routed.bRoutingFallback &&
        routed.selectedModel == "fixture-fast" && fast.Requests() == 1 &&
        routed.routingReason.find("Answered by a remote backend (Ollama") != std::string::npos,
        "With the opt-in, the remote Fast brain did not answer, or the answer did not "
        "say where it came from: " + routed.selectedTier + " / " + routed.routingReason);

    // Main unreachable and Fast remote: memory evaluation stays home rather than
    // sending the exchange out, and says so.
    messageRouter offline;
    offline.ApplyLLMSettings(Settings("LLamaCpp", ClosedPort(), "fixture-main"), fastSettings,
        llmSettings{}, NoEmbeddings(), aiProfile{}, true, false);
    memoryDecision verdict = offline.EvaluateMemory("I like cats.", "Noted.",
        revia::agents::ResponseProvenance::NormalGeneration);
    Check(!verdict.bSuccess && verdict.reason.find("stays on this machine") != std::string::npos &&
        fast.Requests() == 1,
        "Memory evaluation went to a remote brain without the opt-in: " + verdict.reason);
    offline.SetRemotePrivacy(true);
    verdict = offline.EvaluateMemory("I like cats.", "Noted.",
        revia::agents::ResponseProvenance::NormalGeneration);
    Check(verdict.bSuccess && verdict.bShouldRemember && fast.Requests() == 2,
        "With the opt-in, the remote Fast brain did not evaluate memory: " + verdict.reason);
}
} // namespace

void RunProviderBackendTests()
{
    TestCapabilitiesFollowTheBackendName();
    TestAnOpenAiCompatibleServerIsSpokenToInItsOwnDialect();
    TestARemoteTierIsNotHandedPrivateContext();
    std::cout << "An OpenAI-compatible server is spoken to without llama.cpp's extras, a "
                 "llama.cpp profile still insists on /health, and a remote tier gets her "
                 "private context only with the opt-in.\n";
}
