#include "Agents/conversationStylePolicy.h"
#include "LLM/LLamaCPP/llamaCppService.h"
#include "testSupport.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <thread>

namespace
{
using revia::tests::Check;
class ReplyBackend
{
  public:
    ReplyBackend()
    {
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                std::lock_guard lock(mutex);
                lastRequest = nlohmann::json::parse(request.body);
                const nlohmann::json chunk = {
                    {"choices", nlohmann::json::array({{{"delta", {{"content", answer}}}, {"finish_reason", finishReason}}})}};
                response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
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
    void Reply(std::string text, std::string finish = "stop")
    {
        std::lock_guard lock(mutex);
        answer = std::move(text);
        finishReason = std::move(finish);
    }
    nlohmann::json Request()
    {
        std::lock_guard lock(mutex);
        return lastRequest;
    }
    int port = 0;

  private:
    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    std::string answer;
    std::string finishReason;
    nlohmann::json lastRequest;
};

void Configure(llamaCppService& service, int port)
{
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
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
}

void RunStructuredReplyTests()
{
    TestProviderContractAndDataPreservation();
    TestIncompleteDataNeverBecomesSuccessfulProse();
}
