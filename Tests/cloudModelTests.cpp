#include "Audit/cloudRequestAudit.h"
#include "Audit/contentDigest.h"
#include "Core/appSettings.h"
#include "Core/commandManager.h"
#include "Core/configManager.h"
#include "Core/messageRouter.h"
#include "Identity/promptMarkers.h"
#include "LLM/Cloud/cloudChatService.h"
#include "LLM/Cloud/cloudPolicy.h"
#include "LLM/Cloud/cloudProtocol.h"
#include "LLM/Cloud/cloudTransport.h"
#include "testSupport.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// The optional cloud backend, tested without a network: a fake transport records the
// request it would have sent and replays a canned event stream in awkward chunk sizes.

namespace
{
using revia::tests::Check;
using namespace revia::llm::cloud;
using json = nlohmann::json;

constexpr const char* TestKey = "sk-test-cloud-key-0123456789";
constexpr const char* KeyVariable = "REVIA_TEST_CLOUD_KEY";

class FakeTransport final : public CloudTransport
{
public:
    int status = 200;
    std::string body;
    std::size_t chunkSize = 7;
    int calls = 0;
    CloudHttpRequest last;

    CloudHttpResult Post(const CloudHttpRequest& request, const ChunkHandler& onChunk, std::stop_token) override
    {
        ++calls;
        last = request;
        CloudHttpResult result;
        result.bReceived = true;
        result.status = status;
        for (std::size_t at = 0; at < body.size(); at += chunkSize)
        {
            const std::string chunk = body.substr(at, chunkSize);
            result.bytesReceived += chunk.size();
            if (!onChunk(status, chunk))
                break;
        }
        return result;
    }

    [[nodiscard]] std::string Header(const std::string& name) const
    {
        for (const auto& [key, value] : last.headers)
            if (key == name)
                return value;
        return {};
    }
};

struct Harness
{
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::map<std::string, std::string> environment{{KeyVariable, TestKey}};
    std::vector<revia::audit::CloudRequestAuditEntry> audit;
    bool auditWorks = true;
    CloudChatService service;

    explicit Harness(const std::string& provider = "openai-compatible", const std::string& baseUrl = "https://api.example.com/v1")
    {
        CloudModelSettings settings;
        settings.bEnabled = true;
        settings.provider = provider;
        settings.baseUrl = baseUrl;
        settings.model = "test-model";
        settings.apiKeyEnvironmentVariable = KeyVariable;
        settings.useFor = "ask";
        settings.maxTokens = 512;
        service.ApplySettings(settings);
        service.SetPersona("You are Revia.");
        service.SetTransport(transport);
        service.SetEnvironmentReader([this](const std::string& name) -> std::optional<std::string>
        {
            const auto found = environment.find(name);
            return found == environment.end() ? std::nullopt : std::optional<std::string>(found->second);
        });
        service.SetAuditSink([this](const revia::audit::CloudRequestAuditEntry& entry)
        {
            audit.push_back(entry);
            return auditWorks;
        });
    }
};

std::vector<conversationMessage> Ask(const std::string& text)
{
    return {{"user", "Hello there."}, {"assistant", "Hi!"}, {"user", text}};
}

std::string OpenAiStream()
{
    return ": keep-alive\r\n\r\n"
           "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Thinking it over.\"}}]}\r\n\r\n"
           "data: {\"choices\":[{\"delta\":{\"content\":\"Paris is \"}}]}\r\n\r\n"
           "data: {\"choices\":[{\"delta\":{\"content\":\"the capital.\"},\"finish_reason\":\"stop\"}]}\r\n\r\n"
           "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":5}}\r\n\r\n"
           "data: [DONE]\r\n\r\n";
}

std::string AnthropicStream()
{
    return "event: message_start\n"
           "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":40,\"output_tokens\":1}}}\n\n"
           "event: ping\ndata: {\"type\":\"ping\"}\n\n"
           "event: content_block_delta\n"
           "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Hmm.\"}}\n\n"
           "event: content_block_delta\n"
           "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Paris is \"}}\n\n"
           "event: content_block_delta\n"
           "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"the capital.\"}}\n\n"
           "event: message_delta\n"
           "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":7}}\n\n"
           "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
}

void TestEndpointRequiresHttps()
{
    std::string error;
    const auto secure = ParseCloudEndpoint("https://API.Example.com:8443/v1/", error);
    Check(secure && secure->secure && secure->host == "api.example.com" && secure->port == 8443 && secure->basePath == "/v1",
        "An HTTPS base URL was not parsed into host, port and path.");
    const auto loopback = ParseCloudEndpoint("http://127.0.0.1:9090", error);
    Check(loopback && !loopback->secure && loopback->port == 9090, "Plain http to 127.0.0.1 (the test server) was refused.");
    for (const char* refused : {"http://api.example.com/v1", "http://localhost:8080", "ftp://example.com", "api.example.com",
             "https://user:secret@example.com", "https://example.com/v1?key=1", "https://"})
    {
        Check(!ParseCloudEndpoint(refused, error), std::string("An unsafe cloud base URL was accepted: ") + refused);
    }
}

void TestOpenAiRequestShape()
{
    std::string error;
    const CloudEndpoint endpoint = *ParseCloudEndpoint("https://api.example.com/v1", error);
    CloudPayload payload{"System text.", {{"user", "Hi"}, {"assistant", "Hello"}, {"user", "Question?"}}};
    const CloudWireRequest wire = BuildCloudRequest(CloudProvider::OpenAiCompatible, endpoint, "gpt-test", 300, payload, TestKey);
    Check(wire.path == "/v1/chat/completions", "The OpenAI-compatible path is not {base}/chat/completions.");
    bool bearer = false;
    for (const auto& [name, value] : wire.headers)
        bearer = bearer || (name == "Authorization" && value == std::string("Bearer ") + TestKey);
    Check(bearer, "The OpenAI-compatible request does not carry the key as a bearer header.");
    const json body = json::parse(wire.body);
    Check(body["model"] == "gpt-test" && body["max_tokens"] == 300 && body["stream"] == true &&
              body["stream_options"]["include_usage"] == true,
        "The OpenAI-compatible body is missing model, max_tokens or streaming.");
    Check(body["messages"].size() == 4 && body["messages"][0]["role"] == "system" && body["messages"][0]["content"] == "System text." &&
              body["messages"][3]["content"] == "Question?",
        "The OpenAI-compatible messages are not system first, then the turns in order.");
    Check(wire.body.find(TestKey) == std::string::npos, "The API key leaked into the request body.");
}

void TestAnthropicRequestShape()
{
    std::string error;
    const CloudEndpoint endpoint = *ParseCloudEndpoint("https://api.anthropic.com", error);
    // Starts with Revia's own line and has two user turns in a row: both are normal in
    // Revia's history and both would be rejected by the Messages API as they stand.
    CloudPayload payload{"System text.", {{"assistant", "Opening remark."}, {"user", "First."}, {"user", "Second."}, {"assistant", "Reply."},
        {"user", "Third?"}}};
    const CloudWireRequest wire = BuildCloudRequest(CloudProvider::Anthropic, endpoint, "claude-test", 256, payload, TestKey);
    Check(wire.path == "/v1/messages", "The Anthropic path is not {base}/v1/messages.");
    std::map<std::string, std::string> headers(wire.headers.begin(), wire.headers.end());
    Check(headers["x-api-key"] == TestKey && headers["anthropic-version"] == "2023-06-01" && headers.count("Authorization") == 0,
        "The Anthropic request does not use x-api-key and anthropic-version 2023-06-01.");
    const json body = json::parse(wire.body);
    Check(body["model"] == "claude-test" && body["max_tokens"] == 256 && body["stream"] == true && body["system"] == "System text.",
        "The Anthropic body is missing model, max_tokens, stream or the top-level system prompt.");
    const json& messages = body["messages"];
    Check(messages.size() == 3 && messages[0]["role"] == "user" && messages[0]["content"] == "First.\n\nSecond." &&
              messages[1]["role"] == "assistant" && messages[2]["content"] == "Third?",
        "Anthropic messages do not start with the user and alternate.");
    Check(wire.body.find(TestKey) == std::string::npos, "The API key leaked into the request body.");
}

void TestServerSentEventsSurviveAnyChunking()
{
    const std::string stream = "event: a\r\ndata: one\r\ndata: two\r\n\r\n: comment\n\nevent: b\ndata: {\"x\":1}\n\ndata: tail";
    for (std::size_t size = 1; size <= stream.size(); ++size)
    {
        ServerSentEventParser parser;
        std::vector<ServerSentEventParser::Event> events;
        for (std::size_t at = 0; at < stream.size(); at += size)
            for (auto& event : parser.Feed(std::string_view(stream).substr(at, size)))
                events.push_back(std::move(event));
        if (const auto last = parser.Finish())
            events.push_back(*last);
        Check(events.size() == 3, "Server-sent events were lost or invented at chunk size " + std::to_string(size) + ".");
        Check(events[0].name == "a" && events[0].data == "one\ntwo" && events[1].name == "b" && events[1].data == "{\"x\":1}" &&
                  events[2].data == "tail",
            "Server-sent event fields were mis-assembled at chunk size " + std::to_string(size) + ".");
    }
}

void TestStreamEventsDecode()
{
    const auto openAi = DecodeCloudEvent(CloudProvider::OpenAiCompatible,
        {"", "{\"choices\":[{\"delta\":{\"content\":\"Hi\",\"reasoning_content\":\"r\"},\"finish_reason\":\"length\"}]}"});
    Check(openAi.text == "Hi" && openAi.reasoning == "r" && openAi.finishReason == "length", "An OpenAI-compatible delta was misread.");
    Check(DecodeCloudEvent(CloudProvider::OpenAiCompatible, {"", "[DONE]"}).done, "[DONE] did not end the stream.");
    const auto usage = DecodeCloudEvent(CloudProvider::OpenAiCompatible, {"", "{\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4}}"});
    Check(usage.promptTokens == 3U && usage.completionTokens == 4U, "OpenAI-compatible usage was not read.");
    const auto failure = DecodeCloudEvent(CloudProvider::Anthropic,
        {"error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"});
    Check(failure.error == "Overloaded", "An Anthropic error event was not surfaced.");
    Check(DecodeCloudEvent(CloudProvider::Anthropic, {"x", "not json"}).text.empty(), "A malformed event produced text.");
}

void TestStreamedReplyFillsTheResponse(const std::string& provider, const std::string& stream, const std::string& baseUrl)
{
    Harness harness(provider, baseUrl);
    harness.transport->body = stream;
    std::string streamed;
    std::string thought;
    CloudTurnRequest turn;
    turn.bExplicit = true;
    const responseOutput output = harness.service.GenerateResponse(Ask("What is the capital of France?"), turn, {},
        [&streamed](const std::string& delta) { streamed += delta; }, [&thought](const std::string& delta) { thought += delta; });
    Check(output.bSuccess && output.response == "Paris is the capital.", provider + " reply was not assembled: " + output.reason);
    Check(streamed == "Paris is the capital." && output.bWasStreamed, provider + " reply did not stream through the delta handler.");
    Check(!thought.empty() && output.reasoning == thought, provider + " thinking did not reach the reasoning handler.");
    Check(output.bTokensReported && output.promptTokens >= 40 && output.completionTokens >= 5, provider + " token usage was not mapped.");
    Check(output.selectedTier == "Cloud" && output.selectedModel == "cloud:test-model", provider + " reply is not labelled as the cloud's.");
    Check(harness.transport->calls == 1, provider + " sent more or fewer than one request.");
}

void TestKeyComesOnlyFromTheEnvironment()
{
    Harness harness;
    harness.transport->body = OpenAiStream();
    CloudTurnRequest turn;
    turn.bExplicit = true;
    const responseOutput sent = harness.service.GenerateResponse(Ask("Hi?"), turn);
    Check(sent.bSuccess && harness.transport->Header("Authorization") == std::string("Bearer ") + TestKey,
        "The key from the named environment variable was not used.");
    Check(sent.reason.find(TestKey) == std::string::npos && sent.response.find(TestKey) == std::string::npos,
        "The key appeared in the reply or its reason.");
    for (const auto& entry : harness.audit)
        Check(revia::audit::CloudRequestAuditRecord(entry).find(TestKey) == std::string::npos, "The key reached the audit record.");

    harness.environment.clear();
    const responseOutput refused = harness.service.GenerateResponse(Ask("Hi?"), turn);
    Check(!refused.bSuccess && harness.transport->calls == 1 && refused.reason.find(KeyVariable) != std::string::npos,
        "A missing environment variable did not stop the request with a reason that names it.");
    Check(!harness.service.Availability().bAvailable, "The service reported itself available with no key.");

    // The settings file has no key field. One written there anyway is ignored, and the
    // shipped defaults keep the cloud off.
    revia::tests::ScopedTestDirectory directory;
    appSettings shipped;
    Check(configManager("Config/settings.json", "Profiles").LoadSettings(shipped), "The shipped settings no longer load.");
    Check(!shipped.llm.cloud.bEnabled && shipped.llm.cloud.useFor == "never" && shipped.llm.cloud.apiKeyEnvironmentVariable.empty(),
        "The shipped settings do not leave the cloud model off.");
    std::ifstream source("Config/settings.json");
    json document = json::parse(source);
    document["llm"]["cloud"] = {{"enabled", true}, {"provider", "anthropic"}, {"baseUrl", "https://api.anthropic.com"},
        {"model", "m"}, {"apiKeyEnvironmentVariable", "SOME_VAR"}, {"apiKey", TestKey}, {"useFor", "hardTurns"}, {"maxTokens", 2048},
        {"timeoutSeconds", 30}};
    const auto path = directory.root / "settings.json";
    std::ofstream(path) << document.dump(2);
    appSettings loaded;
    Check(configManager(path.string(), "Profiles").LoadSettings(loaded), "A complete llm.cloud block did not load.");
    Check(loaded.llm.cloud.bEnabled && loaded.llm.cloud.provider == "anthropic" && loaded.llm.cloud.useFor == "hardTurns" &&
              loaded.llm.cloud.maxTokens == 2048 && loaded.llm.cloud.timeoutSeconds == 30 &&
              loaded.llm.cloud.apiKeyEnvironmentVariable == "SOME_VAR",
        "llm.cloud fields were not read.");
    document["llm"]["cloud"]["useFor"] = "always";
    std::ofstream(path) << document.dump(2);
    appSettings rejected;
    Check(!configManager(path.string(), "Profiles").LoadSettings(rejected), "An unknown llm.cloud.useFor passed validation.");
}

void TestRefusals()
{
    CloudTurnRequest turn;
    turn.bExplicit = true;
    {
        Harness harness;
        CloudModelSettings off = harness.service.Settings();
        off.bEnabled = false;
        harness.service.ApplySettings(off);
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), turn);
        Check(!output.bSuccess && harness.transport->calls == 0 && harness.audit.empty(), "A disabled cloud model still sent a request.");
    }
    {
        Harness harness;
        CloudModelSettings never = harness.service.Settings();
        never.useFor = "never";
        harness.service.ApplySettings(never);
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), turn);
        Check(!output.bSuccess && harness.transport->calls == 0, "useFor never still sent an explicitly asked turn.");
    }
    {
        Harness harness;
        harness.service.SetSessionEnabled(false);
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), turn);
        Check(!output.bSuccess && harness.transport->calls == 0 && output.reason.find("/cloud on") != std::string::npos,
            "/cloud off did not stop the request.");
    }
    {
        Harness harness;
        CloudTurnRequest shared = turn;
        shared.bPublicAudience = true;
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), shared);
        Check(!output.bSuccess && harness.transport->calls == 0 && harness.audit.empty(), "A public audience reached the cloud model.");
    }
    {
        Harness harness;
        const auto output =
            harness.service.GenerateResponse(Ask("Use this token ghp_abcdefghijklmnopqrstuvwxyz0123456789 to fix CI"), turn);
        Check(!output.bSuccess && harness.transport->calls == 0 && harness.audit.empty(), "A credential in the history was sent.");
        Check(output.response.find("ghp_") == std::string::npos && output.reason.find("GitHub") != std::string::npos,
            "The refusal did not say what was found, or repeated the secret: " + output.reason);
    }
    {
        Harness harness;
        CloudTurnRequest withClipboard = turn;
        withClipboard.bIncludeClipboard = true;
        withClipboard.clipboardText = "AKIAIOSFODNN7EXAMPLE";
        const auto output = harness.service.GenerateResponse(Ask("What did I copy?"), withClipboard);
        Check(!output.bSuccess && harness.transport->calls == 0, "A credential in opted-in clipboard text was sent.");
    }
    {
        Harness harness;
        harness.service.SetAuditSink({});
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), turn);
        Check(!output.bSuccess && harness.transport->calls == 0, "A request was sent with no audit sink.");
    }
    {
        Harness harness;
        harness.auditWorks = false;
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), turn);
        Check(!output.bSuccess && harness.transport->calls == 0, "A request was sent although its audit record failed.");
    }
    {
        Harness harness("openai-compatible", "http://api.example.com/v1");
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), turn);
        Check(!output.bSuccess && harness.transport->calls == 0 && output.reason.find("HTTPS") != std::string::npos,
            "A plain-http cloud endpoint was used.");
    }
    {
        Harness harness("openai-compatible", "http://127.0.0.1:18080/v1");
        harness.transport->body = OpenAiStream();
        const auto output = harness.service.GenerateResponse(Ask("Hi?"), turn);
        Check(output.bSuccess && !harness.transport->last.endpoint.secure && harness.transport->last.endpoint.host == "127.0.0.1",
            "The loopback test endpoint was refused.");
    }
    {
        Harness harness;
        harness.transport->status = 401;
        harness.transport->body = "{\"error\":{\"message\":\"Incorrect API key provided\"}}";
        std::string streamed;
        const auto output =
            harness.service.GenerateResponse(Ask("Hi?"), turn, {}, [&streamed](const std::string& delta) { streamed += delta; });
        Check(!output.bSuccess && streamed.empty() && output.reason.find("HTTP 401") != std::string::npos &&
                  output.reason.find("Incorrect API key") != std::string::npos,
            "An HTTP error was streamed as reply text or lost its explanation.");
    }
}

void TestPrivateContextIsStripped()
{
    namespace markers = revia::identity::markers;
    const std::string screen = "SCREEN: the user has their bank statement open";
    const std::string clipboard = "CLIPTEXT: shopping list";
    const std::string memory = "MEMORY: the user's sister is called Ana";
    const std::string userTurn = std::string(markers::RuntimeTurnContext) + "\n" + std::string(markers::ResponsePosture) + "calm.\n\n" +
        screen + "\n" + std::string(markers::RuntimeTurnContextEnd) + "\n\nWhat should I cook tonight?\n\n" +
        std::string(markers::ClipboardGrounding) + " the text below.\n<<<CLIPBOARD\n" + clipboard + "\nCLIPBOARD>>>\n\n" +
        std::string(markers::RetrievedMemoryBlock) + "\n- " + memory;
    std::vector<conversationMessage> history = {
        {"system", "Hidden system state " + screen}, {"assistant", "Hello!"}, {"user", userTurn}};

    Harness harness;
    harness.transport->body = OpenAiStream();
    CloudTurnRequest turn;
    turn.bExplicit = true;
    turn.screenObservation = screen;
    turn.clipboardText = clipboard;
    turn.privateMemories = memory;
    const auto plain = harness.service.GenerateResponse(history, turn);
    const std::string& sent = harness.transport->last.body;
    Check(plain.bSuccess && sent.find("What should I cook tonight?") != std::string::npos, "The user's own question was not sent.");
    Check(sent.find("SCREEN:") == std::string::npos && sent.find("CLIPTEXT:") == std::string::npos && sent.find("MEMORY:") == std::string::npos,
        "Screen, clipboard or memory left the PC without the per-turn opt-in.");
    Check(sent.find("Hidden system state") == std::string::npos, "A local system message was sent to the cloud.");

    turn.bIncludeScreen = true;
    turn.bIncludeMemories = true;
    (void)harness.service.GenerateResponse(history, turn);
    const std::string& optedIn = harness.transport->last.body;
    Check(optedIn.find("SCREEN:") != std::string::npos && optedIn.find("MEMORY:") != std::string::npos,
        "Opted-in screen and memories were not sent.");
    Check(optedIn.find("CLIPTEXT:") == std::string::npos, "The clipboard was sent although only screen and memories were opted in.");

    std::vector<std::string> stripped;
    (void)StripPrivateContext(userTurn, &stripped);
    Check(stripped.size() >= 3, "The stripped sections were not reported.");
}

void TestAttachmentsStayLocal()
{
    namespace markers = revia::identity::markers;
    const std::string userTurn = "Summarise my lease.\n\n" + std::string(markers::AttachmentGrounding) +
        "\n[lease.pdf]\nFILETEXT: rent is due on the first\n\n" + std::string(markers::DocumentGrounding) +
        "\n[notes.md]\nDOCTEXT: landlord phone number";
    conversationMessage message{"user", userTurn};
    conversationImage picture;
    picture.name = "scan.png";
    picture.mimeType = "image/png";
    picture.bytes = std::make_shared<const std::string>("PIXELDATA");
    message.images.push_back(picture);
    std::vector<conversationMessage> history = {message};

    Harness harness;
    harness.transport->body = OpenAiStream();
    CloudTurnRequest turn;
    turn.bExplicit = true;
    turn.bIncludeScreen = true;
    turn.bIncludeMemories = true;
    const auto reply = harness.service.GenerateResponse(history, turn);
    const std::string& sent = harness.transport->last.body;
    Check(reply.bSuccess && sent.find("Summarise my lease.") != std::string::npos, "The user's own words were not sent.");
    Check(sent.find("FILETEXT:") == std::string::npos && sent.find("DOCTEXT:") == std::string::npos,
        "Attached file or kept document text left the PC.");
    Check(sent.find("PIXELDATA") == std::string::npos && sent.find("UElYRUxEQVRB") == std::string::npos && sent.find("image_url") == std::string::npos,
        "An attached picture left the PC.");

    std::vector<std::string> stripped;
    (void)StripPrivateContext(userTurn, &stripped);
    Check(std::find(stripped.begin(), stripped.end(), "attached files") != stripped.end(), "Stripped attachments were not reported.");
}

void TestEveryRequestIsAudited()
{
    Harness harness;
    harness.transport->body = OpenAiStream();
    CloudTurnRequest turn;
    turn.bExplicit = true;
    const auto output = harness.service.GenerateResponse(Ask("A private question about my week?"), turn);
    Check(output.bSuccess && harness.audit.size() == 2, "A cloud request did not produce an intent and a result audit record.");
    const auto& intent = harness.audit[0];
    const auto& result = harness.audit[1];
    Check(intent.phase == "intent" && result.phase == "result" && intent.transactionId == result.transactionId,
        "The audit records are not an intent and a result of one transaction.");
    Check(intent.host == "api.example.com" && intent.model == "test-model" && intent.provider == "openai-compatible" &&
              intent.requestBytes == harness.transport->last.body.size() &&
              intent.requestDigest == revia::audit::ContentDigest(harness.transport->last.body),
        "The audit does not record host, model, byte count and digest of the exact request.");
    Check(result.succeeded && result.status == 200 && result.responseBytes == OpenAiStream().size(),
        "The result record does not carry status and response size.");
    const std::string line = revia::audit::CloudRequestAuditRecord(result);
    Check(line.find("private question") == std::string::npos && line.find("Paris") == std::string::npos,
        "Conversation or reply text reached the audit record.");
    const json parsed = json::parse(line);
    Check(parsed["host"] == "api.example.com" && parsed["request_digest"] == intent.requestDigest, "The audit JSON lost its fields.");
}

void TestRoutingAndAsks()
{
    const CloudAsk slash = ParseCloudAsk("/cloud ask --screen --memory  What is on my screen?");
    Check(slash.bExplicit && slash.bIncludeScreen && slash.bIncludeMemories && !slash.bIncludeClipboard &&
              slash.question == "What is on my screen?",
        "/cloud ask flags or question were misread.");
    const CloudAsk phrase = ParseCloudAsk("Ask the cloud: why is the sky blue?");
    Check(phrase.bExplicit && phrase.question == "why is the sky blue?" && !phrase.bIncludeScreen, "\"ask the cloud\" was not recognised.");
    Check(!ParseCloudAsk("ask the cloudy forecast").bExplicit && !ParseCloudAsk("what does the cloud say?").bExplicit,
        "Ordinary sentences were taken as cloud requests.");
    Check(!ParseCloudAsk("ask the cloud --screen what is this").bIncludeScreen, "A spoken phrase could opt into sharing the screen.");

    CloudRouteInputs inputs;
    inputs.use = CloudUse::HardTurns;
    inputs.bRouterChoseExpert = true;
    Check(DecideCloudRoute(inputs).bUseCloud && !DecideCloudRoute(inputs).bExplicit, "hardTurns did not take an Expert turn.");
    inputs.use = CloudUse::Ask;
    Check(!DecideCloudRoute(inputs).bUseCloud, "useFor ask took a turn nobody asked the cloud for.");
    inputs.bExplicitAsk = true;
    Check(DecideCloudRoute(inputs).bExplicit, "An explicit ask was not routed to the cloud.");
    inputs.bPublicAudience = true;
    Check(!DecideCloudRoute(inputs).bUseCloud, "A public audience could ask the cloud.");
    inputs.bPublicAudience = false;
    inputs.bExplicitAsk = false;
    inputs.use = CloudUse::Never;
    Check(!DecideCloudRoute(inputs).bUseCloud, "useFor never still routed a hard turn to the cloud.");
    Check(ParseCloudUse("always") == CloudUse::Never, "An unknown useFor did not read as never.");

    Check(commandManager::IsCloudQuestion("/cloud ask what time is it in Tokyo?") && !commandManager::IsCloudQuestion("/cloud ask") &&
              !commandManager::IsCloudQuestion("/cloud off"),
        "/cloud ask with a question is not told apart from the /cloud command.");
    CloudChatService service;
    Check(!service.ToolSupport().bSupported, "The cloud backend claims tool support it does not have.");
}

void TestRouterUsesTheCloudOnlyWhenAsked()
{
    messageRouter router;
    llmSettings settings;
    settings.backend = "Placeholder";
    embeddingSettings embeddings;
    embeddings.bEnabled = false;
    aiProfile profile;
    profile.systemPrompt = "You are Revia.";
    router.ApplyLLMSettings(settings, embeddings, profile);

    auto transport = std::make_shared<FakeTransport>();
    transport->body = AnthropicStream();
    CloudModelSettings cloud;
    cloud.bEnabled = true;
    cloud.provider = "anthropic";
    cloud.baseUrl = "https://api.anthropic.com";
    cloud.model = "claude-test";
    cloud.apiKeyEnvironmentVariable = KeyVariable;
    cloud.useFor = "ask";
    router.Cloud().ApplySettings(cloud);
    router.Cloud().SetTransport(transport);
    router.Cloud().SetEnvironmentReader([](const std::string&) { return std::optional<std::string>(TestKey); });
    router.Cloud().SetAuditSink([](const revia::audit::CloudRequestAuditEntry&) { return true; });

    const auto context = Ask("Capital of France?");
    const responseOutput local = router.RouteMessage("Capital of France?", context);
    Check(local.bSuccess && local.selectedTier != "Cloud" && transport->calls == 0, "A turn went to the cloud without being asked to.");

    CloudTurnRequest explicitAsk;
    explicitAsk.bExplicit = true;
    router.SetCloudTurn(explicitAsk);
    const responseOutput answered = router.RouteMessage("Capital of France?", context);
    Check(answered.bSuccess && answered.selectedTier == "Cloud" && answered.response == "Paris is the capital." && transport->calls == 1,
        "An explicit cloud turn was not answered by the cloud model.");
    const responseOutput after = router.RouteMessage("Capital of France?", context);
    Check(after.selectedTier != "Cloud" && transport->calls == 1, "A cloud request was left behind for the next turn.");

    // An automatic attempt that the cloud refuses falls back to the local route and says so.
    router.Cloud().SetSessionEnabled(false);
    router.SetCloudTurn(CloudTurnRequest{});
    const responseOutput fallback = router.RouteMessage("Capital of France?", context);
    Check(fallback.bSuccess && fallback.selectedTier != "Cloud" && fallback.bRoutingFallback &&
              fallback.routingFallbackReason.find("Cloud model not used") != std::string::npos,
        "A refused automatic cloud attempt did not fall back locally with a note.");
    // An explicit one does not: the user is told why instead of getting a local answer
    // that looks like the cloud's.
    router.SetCloudTurn(explicitAsk);
    const responseOutput refused = router.RouteMessage("Capital of France?", context);
    Check(!refused.bSuccess && refused.selectedTier == "Cloud" && transport->calls == 1,
        "An explicit cloud ask was quietly answered locally while the cloud was switched off.");
}
} // namespace

void RunCloudModelTests()
{
    TestEndpointRequiresHttps();
    TestOpenAiRequestShape();
    TestAnthropicRequestShape();
    TestServerSentEventsSurviveAnyChunking();
    TestStreamEventsDecode();
    TestStreamedReplyFillsTheResponse("openai-compatible", OpenAiStream(), "https://api.example.com/v1");
    TestStreamedReplyFillsTheResponse("anthropic", AnthropicStream(), "https://api.anthropic.com");
    TestKeyComesOnlyFromTheEnvironment();
    TestRefusals();
    TestPrivateContextIsStripped();
    TestAttachmentsStayLocal();
    TestEveryRequestIsAudited();
    TestRoutingAndAsks();
    TestRouterUsesTheCloudOnlyWhenAsked();
}
