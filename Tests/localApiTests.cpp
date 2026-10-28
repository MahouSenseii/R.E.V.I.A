#include "Core/appSettings.h"
#include "Core/configManager.h"
#include "Core/localApiToken.h"
#include "LLM/endpointSettings.h"
#include "Presence/localApiRuntime.h"
#include "testSupport.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
using namespace revia;
using revia::tests::Check;
using Json = nlohmann::json;
using namespace std::chrono_literals;

const std::string Token = "revia-" + std::string(64, 'a');

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A llama-server stand-in: streams each reply in small pieces, records every request,
// and can be held mid-request.
class FakeLlama
{
  public:
    FakeLlama()
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"api-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions",
            [this](const httplib::Request& request, httplib::Response& response)
            {
                {
                    std::lock_guard lock(mutex);
                    requests.push_back(Json::parse(request.body));
                }
                entered = true;
                while (hold)
                    std::this_thread::sleep_for(5ms);
                std::string text = reply;
                if (Contains(request.body, "PRIVACY_PROBE"))
                    text = "Sure. The file is C:/Users/owner/private.txt and more.";
                std::string body;
                for (std::size_t offset = 0; offset < text.size(); offset += 6)
                {
                    const Json event = {{"choices", Json::array({{{"delta", {{"content", text.substr(offset, 6)}}}, {"finish_reason", nullptr}}})}};
                    body += "data: " + event.dump() + "\n\n";
                }
                body += "data: " + Json{{"choices", Json::array({{{"delta", Json::object()}, {"finish_reason", "stop"}}})}}.dump() + "\n\n";
                response.set_content(body + "data: [DONE]\n\n", "text/event-stream");
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(4); };
        port = server.bind_to_any_port("127.0.0.1");
        worker = std::jthread([this] { server.listen_after_bind(); });
        while (!server.is_running())
            std::this_thread::sleep_for(1ms);
    }
    ~FakeLlama()
    {
        hold = false;
        server.stop();
    }
    std::vector<Json> Requests()
    {
        std::lock_guard lock(mutex);
        return requests;
    }

    int port = 0;
    std::string reply = "Hello from Revia. This reply arrives in pieces.";
    std::atomic<bool> hold{false};
    std::atomic<bool> entered{false};

  private:
    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    std::vector<Json> requests;
};

// Sends exactly the bytes it is given and reads the status line, so a body can be
// declared one way and never sent.
class RawClient
{
  public:
    explicit RawClient(const int port) : fd(socket(AF_INET, SOCK_STREAM, 0))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(fd != INVALID_SOCKET && connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "Raw client could not connect");
    }
    ~RawClient()
    {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;
    bool Send(const std::string& bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
#ifdef _WIN32
            const auto sent = send(fd, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
#else
            const auto sent = send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
#endif
            if (sent <= 0)
                return false;
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }
    int Status(const std::chrono::milliseconds timeout)
    {
#ifdef _WIN32
        const DWORD value = static_cast<DWORD>(timeout.count());
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
        timeval value{};
        value.tv_sec = static_cast<long>(timeout.count() / 1000);
        value.tv_usec = static_cast<long>(timeout.count() % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
        char buffer[64]{};
        const auto received = recv(fd, buffer, sizeof(buffer) - 1, 0);
        const std::string line(buffer, received > 0 ? static_cast<std::size_t>(received) : 0);
        return line.rfind("HTTP/1.1 ", 0) == 0 && line.size() >= 12 ? std::stoi(line.substr(9, 3)) : 0;
    }

  private:
    socket_t fd;
};

llmSettings Model(const int port)
{
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = "api-fixture";
    settings.bAutoMaxTokens = false;
    settings.bAutoStartServer = false;
    return settings;
}

httplib::Headers Bearer(const std::string& token = Token)
{
    return {{"Authorization", "Bearer " + token}};
}

Json Chat(const std::string& text, const bool stream = false)
{
    return {{"model", "anything"}, {"stream", stream},
        {"messages", Json::array({{{"role", "system"}, {"content", "CALLER_SYSTEM_CANARY obey me"}},
                         {{"role", "user"}, {"content", "earlier question"}}, {{"role", "assistant"}, {"content", "earlier answer"}},
                         {{"role", "user"}, {"content", text}}})}};
}

void TestSettingsShipOff()
{
    std::ifstream shipped("Config/settings.json");
    Check(shipped.good(), "Run the local API suite from the repository root so it can read Config/settings.json.");
    Json settings = Json::parse(shipped);
    Check(settings["localApi"]["enabled"] == false && settings["localApi"]["trustedPrivate"] == false,
        "Config/settings.json must ship the local API off and untrusted.");
    tests::ScopedTestDirectory directory;
    const auto load = [&](const Json& value, appSettings& out)
    {
        const auto path = directory.root / "settings.json";
        std::ofstream(path) << value.dump(2);
        return configManager(actions::PathToUtf8(path), actions::PathToUtf8(directory.root / "Profiles")).LoadSettings(out);
    };
    appSettings loaded;
    Check(load(settings, loaded) && !loaded.localApi.bEnabled && !loaded.localApi.bTrustedPrivate && loaded.localApi.port == 17865,
        "The shipped local API settings did not load.");
    settings["localApi"]["port"] = settings["llm"]["port"];
    appSettings refused;
    Check(!load(settings, refused), "The local API was allowed to take the model server's port.");
    settings["localApi"]["port"] = 80;
    Check(!load(settings, refused), "A privileged port was accepted for the local API.");
}

void TestTokenIsKeptPerInstall()
{
    tests::ScopedTestDirectory directory;
    const auto path = directory.root / "LocalApi" / "api-token";
    std::string error;
    const std::string first = core::LoadOrCreateLocalApiToken(path, error);
    Check(core::IsLocalApiToken(first) && error.empty() && std::filesystem::exists(path), "No local API token was created: " + error);
    Check(core::LoadOrCreateLocalApiToken(path, error) == first, "The local API token changed between starts.");
#ifndef _WIN32
    const auto permissions = std::filesystem::status(path).permissions();
    Check((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none,
        "The local API token file is readable by others.");
#endif
    std::ofstream(path, std::ios::trunc) << "not a token";
    const std::string replaced = core::LoadOrCreateLocalApiToken(path, error);
    Check(core::IsLocalApiToken(replaced) && replaced != first, "A corrupt token file was not replaced.");
    Check(!core::IsLocalApiToken("revia-short") && !core::IsLocalApiToken("revia-" + std::string(64, 'G')), "A malformed token was accepted.");
}

void TestBoundaryRefusesBeforeReadingTheBody()
{
    FakeLlama llama;
    presence::LocalApiRuntime api(Model(llama.port), [] { return false; });
    Check(!api.Start(0, "short-token"), "A malformed token started the API.");
    Check(api.Start(0, Token), "The local API did not start.");
    const int port = api.Port();
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(10);

    Check(client.Get("/v1/models")->status == 401, "A request without a token was answered.");
    Check(client.Get("/v1/models", Bearer("revia-" + std::string(64, 'b')))->status == 401, "A wrong token was accepted.");
    auto headers = Bearer();
    headers.emplace("Origin", "http://127.0.0.1:" + std::to_string(port));
    Check(client.Get("/v1/models", headers)->status == 403, "A browser Origin was accepted.");
    auto models = client.Get("/v1/models", Bearer());
    Check(models && models->status == 200, "The model list was refused.");
    const Json listed = Json::parse(models->body);
    Check(listed["object"] == "list" && listed["data"][0]["id"] == "revia", "The model list is not in the OpenAI shape.");

    const std::string bearer = "Authorization: Bearer " + Token + "\r\n";
    const auto head = [port](const std::string& extra, const std::string& host = "")
    {
        return "POST /v1/chat/completions HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) +
               "\r\nContent-Type: application/json\r\n" + extra + "\r\n";
    };
    {
        RawClient raw(port);
        raw.Send(head("Transfer-Encoding: chunked\r\n"));
        Check(raw.Status(1500ms) == 401, "An unauthenticated chunked body was not refused at its headers.");
    }
    {
        RawClient raw(port);
        raw.Send(head(bearer + "Transfer-Encoding: chunked\r\n"));
        Check(raw.Status(1500ms) == 400, "A chunked body was accepted.");
    }
    {
        RawClient raw(port);
        raw.Send(head(bearer));
        Check(raw.Status(1500ms) == 400, "A body without a declared length was read.");
    }
    {
        RawClient raw(port);
        raw.Send(head(bearer + "Content-Length: 99999999\r\n"));
        Check(raw.Status(1500ms) == 413, "An oversized body was not refused before it was sent.");
    }
    {
        RawClient raw(port);
        raw.Send(head(bearer + "Content-Length: 10\r\n", "evil.example:" + std::to_string(port)));
        Check(raw.Status(1500ms) == 400, "A foreign Host header was accepted.");
    }
    {
        RawClient raw(port);
        raw.Send("PUT /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n" + bearer + "Content-Length: 4\r\n\r\n");
        Check(raw.Status(1500ms) == 405, "A PUT was not refused.");
    }
    Check(llama.Requests().empty(), "A refused request reached the model.");

    const auto post = [&](const Json& body) { return client.Post("/v1/chat/completions", Bearer(), body.dump(), "application/json"); };
    Json tools = Chat("hi");
    tools["tools"] = Json::array({{{"type", "function"}, {"function", {{"name", "x"}}}}});
    Check(post(tools)->status == 400, "Caller-supplied tools were accepted.");
    Json image = Chat("hi");
    image["messages"].back()["content"] = Json::array({{{"type", "image_url"}, {"image_url", {{"url", "data:x"}}}}});
    Check(post(image)->status == 400, "An image part was silently dropped instead of refused.");
    Json toolRole = Chat("hi");
    toolRole["messages"].push_back({{"role", "tool"}, {"content", "result"}});
    Check(post(toolRole)->status == 400, "A tool message was accepted.");
    Json endsWithAssistant = Chat("hi");
    endsWithAssistant["messages"].push_back({{"role", "assistant"}, {"content", "x"}});
    Check(post(endsWithAssistant)->status == 400, "A request not ending with a user message was accepted.");
    Check(client.Post("/v1/chat/completions", Bearer(), "{not json", "application/json")->status == 400, "Malformed JSON was accepted.");
    Check(llama.Requests().empty(), "An invalid request reached the model.");
    api.Stop();
}

void TestRepliesStreamAndStayShared()
{
    FakeLlama llama;
    std::atomic<bool> ownerBusy = false;
    presence::LocalApiRuntime api(Model(llama.port), [&] { return ownerBusy.load(); });
    Check(api.Start(0, Token) && !api.Trusted(), "The shared local API did not start.");
    httplib::Client client("127.0.0.1", api.Port());
    client.set_read_timeout(20);

    auto response = client.Post("/v1/chat/completions", Bearer(), Chat("What is a maple leaf?").dump(), "application/json");
    Check(response && response->status == 200, "A non-streamed completion failed.");
    const Json completion = Json::parse(response->body);
    Check(completion["object"] == "chat.completion" && completion["model"] == "revia" &&
              completion["choices"][0]["message"]["content"] == llama.reply && completion["choices"][0]["finish_reason"] == "stop",
        "The completion is not in the OpenAI shape: " + response->body);
    const std::string sent = llama.Requests().back().dump();
    Check(Contains(sent, "local API") && !Contains(sent, "CALLER_SYSTEM_CANARY") && Contains(sent, "earlier question") &&
              Contains(sent, "What is a maple leaf?") && !llama.Requests().back().contains("tools"),
        "The shared request did not use Revia's own profile, only the request's turns, and no tools: " + sent);

    response = client.Post("/v1/chat/completions", Bearer(), Chat("Stream it.", true).dump(), "application/json");
    Check(response && response->status == 200 && Contains(response->get_header_value("Content-Type"), "text/event-stream"),
        "A streamed completion did not answer with an event stream.");
    std::string joined;
    int contentEvents = 0;
    std::string finish;
    std::size_t start = 0;
    bool done = false;
    while ((start = response->body.find("data: ", start)) != std::string::npos)
    {
        const std::size_t end = response->body.find("\n\n", start);
        const std::string data = response->body.substr(start + 6, end - start - 6);
        start = end;
        if (data == "[DONE]")
        {
            done = true;
            continue;
        }
        const Json event = Json::parse(data);
        Check(event["object"] == "chat.completion.chunk", "A streamed event is not a completion chunk.");
        const Json& choice = event["choices"][0];
        if (choice["delta"].contains("content") && !choice["delta"]["content"].get<std::string>().empty())
        {
            joined += choice["delta"]["content"].get<std::string>();
            ++contentEvents;
        }
        if (!choice["finish_reason"].is_null())
            finish = choice["finish_reason"].get<std::string>();
    }
    Check(done && finish == "stop" && joined == llama.reply && contentEvents >= 2,
        "The reply did not stream sentence by sentence through the gate: " + std::to_string(contentEvents) + " events, " + joined);

    // Private details never leave, streamed or not.
    response = client.Post("/v1/chat/completions", Bearer(), Chat("PRIVACY_PROBE").dump(), "application/json");
    Check(response && response->status == 200 && !Contains(response->body, "C:/Users") &&
              Contains(response->body, "I can't share private local details through the local API."),
        "A private path in the reply reached the caller.");
    response = client.Post("/v1/chat/completions", Bearer(), Chat("PRIVACY_PROBE", true).dump(), "application/json");
    Check(response && response->status == 200 && !Contains(response->body, "C:/Users"), "A private path was streamed to the caller.");

    // The owner comes first: a busy owner refuses new requests and cancels one in flight.
    ownerBusy = true;
    Check(client.Post("/v1/chat/completions", Bearer(), Chat("hi").dump(), "application/json")->status == 429,
        "A request was admitted while the owner was busy.");
    ownerBusy = false;
    llama.hold = true;
    llama.entered = false;
    std::atomic<int> cancelledStatus = 0;
    std::thread waiting(
        [&]
        {
            httplib::Client second("127.0.0.1", api.Port());
            second.set_read_timeout(20);
            const auto cancelled = second.Post("/v1/chat/completions", Bearer(), Chat("long one").dump(), "application/json");
            cancelledStatus = cancelled ? cancelled->status : -1;
        });
    while (!llama.entered)
        std::this_thread::sleep_for(5ms);
    Check(client.Post("/v1/chat/completions", Bearer(), Chat("overlap").dump(), "application/json")->status == 429,
        "Two requests ran at once.");
    api.Preempt();
    llama.hold = false;
    waiting.join();
    Check(cancelledStatus == 503, "A preempted request was not reported as cancelled: " + std::to_string(cancelledStatus.load()));
    api.Stop();
}

void TestTrustedModeIsAnOwnerTurn()
{
    FakeLlama llama;
    std::mutex mutex;
    std::vector<std::string> turns;
    presence::LocalApiRuntime api(Model(llama.port), [] { return false; });
    Check(api.Start(0, Token,
              [&](const std::string& input, std::stop_token)
              {
                  std::lock_guard lock(mutex);
                  turns.push_back(input);
                  runtime::SessionResult result;
                  result.text = "Owner turn answered.";
                  return result;
              }) &&
              api.Trusted(),
        "The trusted local API did not start.");
    httplib::Client client("127.0.0.1", api.Port());
    client.set_read_timeout(10);
    auto response = client.Post("/v1/chat/completions", Bearer(), Chat("Remember that I like tea.").dump(), "application/json");
    Check(response && response->status == 200 && Json::parse(response->body)["choices"][0]["message"]["content"] == "Owner turn answered.",
        "A trusted request was not answered by the owner's turn.");
    response = client.Post("/v1/chat/completions", Bearer(), Chat("Stream the owner turn.", true).dump(), "application/json");
    Check(response && Contains(response->body, "Owner turn answered.") && Contains(response->body, "[DONE]"), "A trusted stream failed.");
    Check(client.Post("/v1/chat/completions", Bearer(), Chat("/internet on").dump(), "application/json")->status == 400,
        "A command was accepted through the trusted API.");
    Check(client.Post("/v1/chat/completions", Bearer(), Chat("bye").dump(), "application/json")->status == 400,
        "An exit word was accepted through the trusted API.");
    std::lock_guard lock(mutex);
    Check(turns == std::vector<std::string>{"Remember that I like tea.", "Stream the owner turn."} && llama.Requests().empty(),
        "The trusted API used the shared router, or ran a command.");
    api.Stop();
}

void TestEditorObservationsRequireTrustedCallback()
{
    FakeLlama llama;
    std::atomic<int> ownerTurns = 0;
    for (const bool trusted : {false, true})
    {
        presence::LocalApiRuntime api(Model(llama.port), [] { return false; });
        presence::LocalApiRuntime::PrivateTurn owner;
        if (trusted)
            owner = [&](const std::string&, std::stop_token) { ++ownerTurns; return runtime::SessionResult{}; };
        Check(api.Start(0, Token, owner), "The editor boundary fixture did not start.");
        httplib::Client client("127.0.0.1", api.Port());
        client.set_read_timeout(5);
        const Json saved = {{"source", "saved_file"}, {"projectRoot", "C:/fixture"}, {"path", "C:/fixture/code.cpp"},
            {"revision", std::string(64, 'a')}, {"occurredAtUnixMs", 1793206800000LL}};
        const auto payload = saved.dump();
        {
            RawClient raw(api.Port());
            Check(raw.Send("POST /v1/editor/observations HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(api.Port()) +
                    "\r\nAuthorization: Bearer " + Token + "\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(payload.size()) + "\r\n\r\n"), "The gated editor request headers were not sent.");
            const int wire = raw.Status(1500ms);
            const bool bodySent = raw.Send(payload);
            std::cout << "Editor absent-callback wire: trusted=" << trusted << " status=" << wire
                      << " body_attempted=1 body_sent=" << bodySent << " owner_turns=" << ownerTurns.load()
                      << " inference_requests=" << llama.Requests().size() << std::endl;
            Check(wire == 403 && ownerTurns == 0 && llama.Requests().empty(),
                "The nonempty editor challenge lacked an actual wire 403 or caused a callback/model effect.");
        }
        // Keep the ordinary parser assertion independent of the gated nonempty wire challenge.
        const auto response = client.Post("/v1/editor/observations", Bearer(), "", "application/json");
        Check(response && response->status == 403,
            "An editor observation without a trusted callback was not refused: " +
                (response ? std::to_string(response->status) + " " + response->body : httplib::to_string(response.error())));
        api.Stop();
        Check(ownerTurns == 0 && llama.Requests().empty(), "An absent editor callback caused an owner turn or inference.");
    }
}

void TestEditorObservationsAreBoundedAndDoNotGenerate()
{
    FakeLlama llama;
    std::atomic<int> calls = 0;
    bool accept = true;
    bool fail = false;
    bool retryable = false;
    presence::EditorObservationRequest received;
    const auto callback = [&](const presence::EditorObservationRequest& saved)
    {
        ++calls;
        received = saved;
        if (fail)
            throw std::runtime_error("PRIVATE_EXCEPTION_CANARY");
        return presence::EditorObservationResult{accept, accept ? "Saved observation accepted." : "Source is paused.", "saved-event-1", retryable};
    };
    const auto owner = [](const std::string&, std::stop_token) { return runtime::SessionResult{}; };
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const Json saved = {{"source", "saved_file"}, {"projectRoot", "C:/fixture"}, {"path", "C:/fixture/code.cpp"},
        {"revision", std::string(64, 'a')}, {"occurredAtUnixMs", now}};
    {
        presence::LocalApiRuntime shared(Model(llama.port), [] { return false; });
        Check(shared.Start(0, Token, {}, callback), "The shared editor fixture did not start.");
        httplib::Client client("127.0.0.1", shared.Port());
        const auto payload = saved.dump();
        {
            RawClient raw(shared.Port());
            Check(raw.Send("POST /v1/editor/observations HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(shared.Port()) +
                    "\r\nAuthorization: Bearer " + Token + "\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(payload.size()) + "\r\n\r\n"), "The shared editor challenge headers were not sent.");
            const int wire = raw.Status(1500ms);
            const bool bodySent = raw.Send(payload);
            std::cout << "Editor shared-callback wire: status=" << wire << " body_attempted=1 body_sent=" << bodySent
                      << " callbacks=" << calls.load() << " inference_requests=" << llama.Requests().size() << std::endl;
            Check(wire == 403 && calls == 0 && llama.Requests().empty(), "Shared editor authority was not refused on the wire before its body.");
        }
        const auto response = client.Post("/v1/editor/observations", Bearer(), "", "application/json");
        Check(response && response->status == 403 && calls == 0, "A callback granted shared-mode editor authority.");
        shared.Stop();
    }
    presence::LocalApiRuntime api(Model(llama.port), [] { return false; });
    Check(api.Start(0, Token, owner, callback), "The trusted editor fixture did not start.");
    httplib::Client client("127.0.0.1", api.Port());
    client.set_read_timeout(5);
    const auto post = [&](const Json& body) { return client.Post("/v1/editor/observations", Bearer(), body.dump(), "application/json"); };
    const auto wrongTokenValue = "revia-" + std::string(64, 'b');
    {
        const auto payload = saved.dump();
        RawClient raw(api.Port());
        Check(raw.Send("POST /v1/editor/observations HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(api.Port()) +
                "\r\nAuthorization: Bearer " + wrongTokenValue + "\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(payload.size()) + "\r\n\r\n"), "The wrong-token editor challenge headers were not sent.");
        const int wire = raw.Status(1500ms);
        const bool bodySent = raw.Send(payload);
        std::cout << "Editor wrong-token wire: status=" << wire << " body_attempted=1 body_sent=" << bodySent
                  << " callbacks=" << calls.load() << " inference_requests=" << llama.Requests().size() << std::endl;
        Check(wire == 401 && calls == 0 && llama.Requests().empty(), "The wrong editor token lacked wire401 or caused an effect.");
    }
    const auto wrongToken = client.Post("/v1/editor/observations", Bearer(wrongTokenValue), "", "application/json");
    Check(wrongToken && wrongToken->status == 401, "An incorrect editor token did not produce a parsed 401: " +
        (wrongToken ? std::to_string(wrongToken->status) : httplib::to_string(wrongToken.error())));
    auto origin = Bearer();
    origin.emplace("Origin", "http://127.0.0.1");
    {
        const auto payload = saved.dump();
        RawClient raw(api.Port());
        Check(raw.Send("POST /v1/editor/observations HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(api.Port()) +
                "\r\nAuthorization: Bearer " + Token + "\r\nOrigin: http://127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(payload.size()) + "\r\n\r\n"), "The editor Origin challenge headers were not sent.");
        const int wire = raw.Status(1500ms);
        const bool bodySent = raw.Send(payload);
        std::cout << "Editor Origin wire: status=" << wire << " body_attempted=1 body_sent=" << bodySent
                  << " callbacks=" << calls.load() << " inference_requests=" << llama.Requests().size() << std::endl;
        Check(wire == 403 && calls == 0 && llama.Requests().empty(), "The editor Origin challenge lacked wire403 or caused an effect.");
    }
    const auto browser = client.Post("/v1/editor/observations", origin, "", "application/json");
    Check(browser && browser->status == 403, "An editor browser Origin did not produce a parsed 403: " +
        (browser ? std::to_string(browser->status) : httplib::to_string(browser.error())));
    Check(client.Post("/v1/editor/observations", Bearer(), "{", "application/json")->status == 400, "Malformed editor JSON was accepted.");
    Check(client.Post("/v1/editor/observations", Bearer(), saved.dump(), "text/plain")->status == 400, "Non-JSON editor input was accepted.");
    Check(client.Post("/v1/editor/observations", Bearer(), saved.dump(), "application/jsonp")->status == 400,
        "A JSON-prefix media type was accepted for editor observations.");
    std::vector<Json> invalid;
    auto changed = saved;
    changed["text"] = "Client supplied code or commands must never become an observation.";
    invalid.push_back(changed);
    for (const auto& source : {Json("unsaved_buffer"), Json("screen"), Json(1)})
    {
        changed = saved;
        changed["source"] = source;
        invalid.push_back(changed);
    }
    for (const auto& path : {Json(""), Json(std::string(1025, 'x')), Json(std::string("bad\0path", 8)), Json(3)})
    {
        changed = saved;
        changed["path"] = path;
        invalid.push_back(changed);
    }
    changed = saved;
    changed["projectRoot"] = "";
    invalid.push_back(changed);
    for (const auto& revision : {Json(std::string(63, 'a')), Json(std::string(64, 'g')), Json(17)})
    {
        changed = saved;
        changed["revision"] = revision;
        invalid.push_back(changed);
    }
    for (const auto& time : {Json("now"), Json(1.5), Json(0), Json(now - 600000), Json(now + 120000), Json(UINT64_MAX)})
    {
        changed = saved;
        changed["occurredAtUnixMs"] = time;
        invalid.push_back(changed);
    }
    changed = saved;
    changed.erase("revision");
    invalid.push_back(changed);
    invalid.push_back(Json::array());
    for (const auto& value : invalid)
    {
        const auto response = post(value);
        Check(response && response->status == 400, "An invalid saved-file observation was accepted: " + value.dump());
    }
    auto duplicate = saved.dump();
    duplicate.insert(1, "\"source\":\"saved_file\",");
    Check(client.Post("/v1/editor/observations", Bearer(), duplicate, "application/json")->status == 400,
        "A duplicate editor JSON key was accepted.");
    const auto headers = [&](const std::string& fields, const std::string& host = "")
    {
        return "POST /v1/editor/observations HTTP/1.1\r\nHost: " +
               (host.empty() ? "127.0.0.1:" + std::to_string(api.Port()) : host) + "\r\nAuthorization: Bearer " + Token +
               "\r\nContent-Type: application/json\r\n" + fields + "\r\n";
    };
    for (const auto& test : std::vector<std::pair<std::string, int>>{{"Content-Length: 8193\r\n", 413},
             {"Transfer-Encoding: chunked\r\n", 400}, {"", 400}})
    {
        RawClient raw(api.Port());
        raw.Send(headers(test.first));
        Check(raw.Status(1500ms) == test.second, "An editor body was not refused at its headers.");
    }
    {
        RawClient raw(api.Port());
        raw.Send(headers("Content-Length: 10\r\n", "foreign.example:" + std::to_string(api.Port())));
        Check(raw.Status(1500ms) == 400, "A foreign Host reached editor observations.");
    }
    Check(calls == 0 && llama.Requests().empty(), "Rejected editor observations reached the callback or model.");
    auto response = post(saved);
    Check(response && response->status == 202 && Json::parse(response->body)["eventId"] == "saved-event-1" && calls == 1 &&
              received.projectRoot == "C:/fixture" && received.path == "C:/fixture/code.cpp" &&
              received.revision == std::string(64, 'a') && received.occurredAtUnixMs == now,
        "A valid editor observation did not reach the typed callback exactly once.");
    accept = false;
    response = post(saved);
    Check(response && response->status == 409 && Json::parse(response->body)["accepted"] == false &&
              Json::parse(response->body)["eventId"] == "" && Json::parse(response->body)["retryable"] == false && calls == 2,
        "A rejected editor observation was reported as accepted.");
    retryable = true;
    response = post(saved);
    Check(response && response->status == 409 && Json::parse(response->body)["retryable"] == true && calls == 3,
        "A transient busy observation lost its host-owned retry signal.");
    fail = true;
    response = post(saved);
    Check(response && response->status == 503 && !Contains(response->body, "PRIVATE_EXCEPTION_CANARY") && calls == 4 && llama.Requests().empty(),
        "An editor callback failure disclosed diagnostics or generated a model request.");
    api.Stop();
}
} // namespace

void RunLocalApiTests()
{
    TestSettingsShipOff();
    TestTokenIsKeptPerInstall();
    TestEditorObservationsRequireTrustedCallback();
    TestEditorObservationsAreBoundedAndDoNotGenerate();
    TestBoundaryRefusesBeforeReadingTheBody();
    TestRepliesStreamAndStayShared();
    TestTrustedModeIsAnOwnerTurn();
}
