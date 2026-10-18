#include "Presence/localApiRuntime.h"

#include "Core/conversationMessage.h"
#include "Core/localApiKey.h"
#include "Core/localApiToken.h"
#include "Core/messageRouter.h"
#include "Core/utf8.h"
#include "Runtime/conversationRuntime.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace revia::presence
{
namespace
{
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t MaximumMessages = 64;
constexpr std::size_t MaximumInputBytes = 8192;
// What a shared request may carry as earlier turns: the same bounds as a web guest.
constexpr std::size_t MaximumHistoryMessages = 8;
constexpr std::size_t MaximumHistoryBytes = 32768;
constexpr auto RequestDeadline = 120s;

long long UnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Respond(httplib::Response& response, const Json& body, const int status = 200)
{
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(body.dump(-1, ' ', false, Json::error_handler_t::replace), "application/json");
}

// OpenAI's error shape, so existing clients show the message.
void Error(httplib::Response& response, const int status, const std::string& message, const std::string& type, const std::string& code)
{
    Respond(response, {{"error", {{"message", message}, {"type", type}, {"code", code}}}}, status);
}

struct ParsedRequest
{
    std::string input;
    std::vector<conversationMessage> history;
    bool stream = false;
    std::string error;
};

// Text of one message: a string, or a list of text parts. Anything else is refused
// rather than silently dropped, so a client never believes an image was seen.
bool MessageText(const Json& content, std::string& text)
{
    text.clear();
    if (content.is_string())
    {
        text = content.get<std::string>();
        return true;
    }
    if (!content.is_array())
        return false;
    for (const auto& part : content)
    {
        if (!part.is_object() || part.value("type", std::string{}) != "text" || !part.contains("text") || !part["text"].is_string())
            return false;
        text += (text.empty() ? "" : "\n") + part["text"].get<std::string>();
    }
    return true;
}

ParsedRequest Parse(const Json& body)
{
    ParsedRequest parsed;
    if (!body.is_object() || !body.contains("messages") || !body["messages"].is_array() || body["messages"].empty() ||
        body["messages"].size() > MaximumMessages)
    {
        parsed.error = "\"messages\" must be a list of 1 to 64 messages.";
        return parsed;
    }
    if (body.contains("stream") && !body["stream"].is_boolean() && !body["stream"].is_null())
    {
        parsed.error = "\"stream\" must be true or false.";
        return parsed;
    }
    parsed.stream = body.value("stream", false) == true;
    if (body.contains("n") && !body["n"].is_null() && body["n"] != 1)
    {
        parsed.error = "Only one choice (n = 1) is generated.";
        return parsed;
    }
    for (const char* key : {"tools", "functions"})
    {
        if (body.contains(key) && !body[key].is_null() && !(body[key].is_array() && body[key].empty()))
        {
            parsed.error = "Tools and functions are not available through Revia's local API.";
            return parsed;
        }
    }
    std::vector<conversationMessage> turns;
    for (const auto& message : body["messages"])
    {
        if (!message.is_object() || !message.contains("role") || !message["role"].is_string())
        {
            parsed.error = "Every message needs a role.";
            return parsed;
        }
        const std::string role = message["role"].get<std::string>();
        // Revia keeps her own instructions; a calling program's system prompt is not one
        // of them.
        if (role == "system" || role == "developer")
            continue;
        if (role != "user" && role != "assistant")
        {
            parsed.error = "Messages with the role \"" + utf8::Prefix(utf8::Sanitize(role), 32) + "\" are not supported.";
            return parsed;
        }
        if (message.contains("tool_calls") || message.contains("function_call"))
        {
            parsed.error = "Tool calls are not available through Revia's local API.";
            return parsed;
        }
        std::string text;
        if (!message.contains("content") || !MessageText(message["content"], text))
        {
            parsed.error = "Only text content is supported.";
            return parsed;
        }
        if (text.find('\0') != std::string::npos || !utf8::IsValid(text))
        {
            parsed.error = "Message text must be valid UTF-8.";
            return parsed;
        }
        turns.push_back({role, std::move(text)});
    }
    if (turns.empty() || turns.back().role != "user" || turns.back().content.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        parsed.error = "The last message must be a non-empty user message.";
        return parsed;
    }
    if (turns.back().content.size() > MaximumInputBytes)
    {
        parsed.error = "The last message is longer than 8192 bytes.";
        return parsed;
    }
    parsed.input = turns.back().content;
    turns.pop_back();
    // The newest earlier turns that fit, oldest dropped first.
    std::size_t bytes = 0;
    for (auto turn = turns.rbegin(); turn != turns.rend() && parsed.history.size() < MaximumHistoryMessages; ++turn)
    {
        bytes += turn->content.size();
        if (bytes > MaximumHistoryBytes)
            break;
        parsed.history.insert(parsed.history.begin(), *turn);
    }
    return parsed;
}

Json Chunk(const std::string& id, const long long created, const Json& delta, const Json& finish)
{
    return {{"id", id}, {"object", "chat.completion.chunk"}, {"created", created}, {"model", LocalApiRuntime::ModelId},
        {"choices", Json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
}
} // namespace

struct LocalApiRuntime::Impl
{
    struct Work
    {
        std::stop_source stop;
        Clock::time_point deadline = Clock::now() + RequestDeadline;
        bool trusted = false;
    };

    llmSettings model;
    std::function<bool()> ownerBusy;
    PrivateTurn privateTurn;
    messageRouter router;
    httplib::Server server;
    std::jthread listener;
    std::jthread maintenance;
    std::mutex mutex;
    std::shared_ptr<Work> active;
    std::string token;
    std::atomic<int> port{0};
    long long started = UnixSeconds();

    Impl(const llmSettings& settings, std::function<bool()> busy) : model(settings), ownerBusy(std::move(busy))
    {
        // Model transport and inference only; never the owner's profile, memory or vision.
        model.backend = "LLamaCpp";
        model.bAutoStartServer = false;
        model.bVisionEnabled = false;
        model.bAutoMaxTokens = false;
        model.maxTokens = 1024;
        model.parallelRequests = 1;
        model.contextSize = std::min(8192, model.contextSize);
        router.ApplyLLMSettings(model, {}, SharedProfile());
    }

    [[nodiscard]] bool Busy() const
    {
        return ownerBusy && ownerBusy();
    }

    bool Authenticate(const httplib::Request& request, httplib::Response& response)
    {
        // Constant time over the expected length, so the comparison does not reveal how
        // much of a guess was right.
        const std::string supplied = request.get_header_value("Authorization");
        const std::string expected = "Bearer " + token;
        unsigned int difference = static_cast<unsigned int>(supplied.size() ^ expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
            difference |= static_cast<unsigned char>(expected[index]) ^ (index < supplied.size() ? static_cast<unsigned char>(supplied[index]) : 0);
        if (difference != 0 || token.empty())
        {
            Error(response, 401, "A valid bearer token is required.", "invalid_request_error", "invalid_api_key");
            return false;
        }
        // A web page must never reach this API, including one on this machine.
        if (request.has_header("Origin"))
        {
            Error(response, 403, "Browser requests are refused.", "invalid_request_error", "forbidden");
            return false;
        }
        return true;
    }

    // A Host other than this address is a request that was not meant for Revia, such as
    // a DNS-rebinding page.
    [[nodiscard]] bool LoopbackHost(const httplib::Request& request) const
    {
        const std::string host = request.get_header_value("Host");
        const std::string suffix = ":" + std::to_string(port.load());
        return host == "127.0.0.1" + suffix || host == "localhost" + suffix;
    }

    std::shared_ptr<Work> Admit(const bool trusted)
    {
        std::lock_guard lock(mutex);
        if (active || Busy())
            return nullptr;
        active = std::make_shared<Work>();
        active->trusted = trusted;
        return active;
    }

    void Release(const std::shared_ptr<Work>& work)
    {
        std::lock_guard lock(mutex);
        if (active == work)
            active.reset();
    }

    runtime::SessionResult Answer(const ParsedRequest& request, const std::shared_ptr<Work>& work, const messageRouter::DeltaHandler& onDelta)
    {
        runtime::SessionResult result;
        result.succeeded = false;
        try
        {
            if (work->trusted && privateTurn)
                result = privateTurn(request.input, work->stop.get_token());
            else
                result = runtime::ConversationRuntime::ReplyIsolated(router, request.input, request.history, work->stop.get_token(), onDelta,
                    "I can't share private local details through the local API.");
        }
        catch (...)
        {
            // Diagnostics never cross this boundary.
            result.succeeded = false;
        }
        return result;
    }

    void Completions(const httplib::Request& request, httplib::Response& response)
    {
        if (request.get_header_value("Content-Type").find("application/json") != 0)
        {
            Error(response, 400, "The body must be application/json.", "invalid_request_error", "invalid_content_type");
            return;
        }
        Json body;
        try
        {
            body = Json::parse(request.body);
        }
        catch (...)
        {
            Error(response, 400, "The body is not valid JSON.", "invalid_request_error", "invalid_json");
            return;
        }
        auto parsed = std::make_shared<ParsedRequest>(Parse(body));
        if (!parsed->error.empty())
        {
            Error(response, 400, parsed->error, "invalid_request_error", "invalid_request");
            return;
        }
        const bool trusted = static_cast<bool>(privateTurn);
        // A trusted request is an owner turn, and an owner turn starting with a slash, or
        // one of the exit words, is a command. Commands stay with the person at the keyboard.
        const std::size_t first = parsed->input.find_first_not_of(" \t\r\n");
        if (trusted && first != std::string::npos &&
            (parsed->input[first] == '/' || parsed->input == "exit" || parsed->input == "quit" || parsed->input == "bye"))
        {
            Error(response, 400, "Commands are not accepted through the local API.", "invalid_request_error", "command_refused");
            return;
        }
        const auto work = Admit(trusted);
        if (!work)
        {
            Error(response, 429, "Revia is busy with her owner or another request. Try again shortly.", "server_busy", "busy");
            return;
        }
        const std::string id = "chatcmpl-" + core::GenerateLocalApiKey().substr(6, 24);
        const long long created = UnixSeconds();
        if (!parsed->stream)
        {
            const auto result = Answer(*parsed, work, {});
            const bool cancelled = work->stop.stop_requested();
            Release(work);
            if (cancelled)
            {
                Error(response, 503, "The request was cancelled: Revia's owner needed her, or it took too long.", "server_busy", "cancelled");
                return;
            }
            if (!result.succeeded || result.text.empty())
            {
                Error(response, 502, "Revia could not answer.", "server_error", "no_reply");
                return;
            }
            Respond(response, {{"id", id}, {"object", "chat.completion"}, {"created", created}, {"model", ModelId},
                                  {"choices", Json::array({{{"index", 0}, {"message", {{"role", "assistant"}, {"content", result.text}}},
                                                  {"finish_reason", "stop"}}})}});
            return;
        }

        // Streaming: the reply runs inside the first call for content, on the connection's
        // own thread, and every approved delta is written as it is released. Nothing the
        // gate has not approved is ever written.
        auto released = std::make_shared<std::atomic<bool>>(false);
        const auto release = [this, work, released]()
        {
            if (!released->exchange(true))
                Release(work);
        };
        response.set_header("Cache-Control", "no-store");
        response.set_chunked_content_provider(
            "text/event-stream",
            [this, parsed, work, id, created, release](std::size_t, httplib::DataSink& sink)
            {
                bool open = true;
                const auto write = [&](const std::string& event)
                {
                    if (!open)
                        return false;
                    const std::string line = "data: " + event + "\n\n";
                    if (!sink.write(line.data(), line.size()))
                    {
                        // The client went away; the reply stops with it.
                        open = false;
                        work->stop.request_stop();
                    }
                    return open;
                };
                const auto chunk = [&](const Json& delta, const Json& finish)
                { return write(Chunk(id, created, delta, finish).dump(-1, ' ', false, Json::error_handler_t::replace)); };
                chunk({{"role", "assistant"}, {"content", ""}}, nullptr);
                std::string streamed;
                const auto result = Answer(*parsed, work,
                    [&](const std::string& delta)
                    {
                        if (delta.empty() || work->stop.stop_requested())
                            return;
                        streamed += delta;
                        chunk({{"content", delta}}, nullptr);
                    });
                if (work->stop.stop_requested())
                {
                    write(Json{{"error", {{"message", "The request was cancelled: Revia's owner needed her, or it took too long."},
                                             {"type", "server_busy"}, {"code", "cancelled"}}}}
                              .dump());
                }
                else if (!result.succeeded || result.text.empty())
                {
                    write(Json{{"error", {{"message", "Revia could not answer."}, {"type", "server_error"}, {"code", "no_reply"}}}}.dump());
                }
                else if (result.text.compare(0, streamed.size(), streamed) == 0)
                {
                    // The final pass kept everything already sent; the rest follows.
                    if (result.text.size() > streamed.size())
                        chunk({{"content", result.text.substr(streamed.size())}}, nullptr);
                    chunk(Json::object(), "stop");
                }
                else
                {
                    // The final pass changed text already sent, which cannot be taken back.
                    // The stream ends without the changed reply, marked as filtered.
                    chunk(Json::object(), "content_filter");
                }
                write("[DONE]");
                release();
                if (open)
                    sink.done();
                return open;
            },
            [release](bool) { release(); });
    }

    void Routes()
    {
        server.set_payload_max_length(MaximumBodyBytes);
        server.set_read_timeout(5);
        server.set_write_timeout(10);
        server.set_keep_alive_max_count(8);
        server.new_task_queue = [] { return new httplib::ThreadPool(4, 16); };
        // cpp-httplib reads the whole body before a route runs, and a chunked or
        // length-less body ignores the payload limit. This runs after the headers and
        // before the body, as the web guest's does: it authenticates and admits only a
        // declared, bounded Content-Length.
        server.set_pre_routing_handler(
            [this](const httplib::Request& request, httplib::Response& response)
            {
                using Outcome = httplib::Server::HandlerResponse;
                if (request.method != "GET" && request.method != "POST")
                {
                    Error(response, 405, "Only GET and POST are supported.", "invalid_request_error", "method_not_allowed");
                    return Outcome::Handled;
                }
                if (!LoopbackHost(request))
                {
                    Error(response, 400, "The Host header must name this loopback address.", "invalid_request_error", "invalid_host");
                    return Outcome::Handled;
                }
                if (!Authenticate(request, response))
                    return Outcome::Handled;
                if (request.method == "POST")
                {
                    if (request.has_header("Transfer-Encoding") || !request.has_header("Content-Length"))
                    {
                        Error(response, 400, "The body must have a declared Content-Length.", "invalid_request_error", "length_required");
                        return Outcome::Handled;
                    }
                    const std::string length = request.get_header_value("Content-Length");
                    const bool digits = !length.empty() && length.size() <= 7 &&
                                        std::all_of(length.begin(), length.end(), [](const unsigned char c) { return c >= '0' && c <= '9'; });
                    if (!digits || std::stoul(length) > MaximumBodyBytes)
                    {
                        Error(response, 413, "The body is larger than 64 KiB.", "invalid_request_error", "payload_too_large");
                        return Outcome::Handled;
                    }
                }
                return Outcome::Unhandled;
            });
        server.Get("/v1/models",
            [this](const httplib::Request&, httplib::Response& response)
            {
                Respond(response, {{"object", "list"},
                                      {"data", Json::array({{{"id", ModelId}, {"object", "model"}, {"created", started}, {"owned_by", "revia"}}})}});
            });
        server.Post("/v1/chat/completions", [this](const httplib::Request& request, httplib::Response& response) { Completions(request, response); });
        server.set_error_handler(
            [](const httplib::Request&, httplib::Response& response)
            {
                if (response.body.empty())
                    Error(response, response.status, "Not found.", "invalid_request_error", "not_found");
            });
        server.set_exception_handler([](const httplib::Request&, httplib::Response& response, std::exception_ptr)
            { Error(response, 503, "Revia could not answer.", "server_error", "unavailable"); });
    }
};

LocalApiRuntime::LocalApiRuntime(const llmSettings& model, std::function<bool()> ownerBusy)
    : impl(std::make_unique<Impl>(model, std::move(ownerBusy)))
{
}

LocalApiRuntime::~LocalApiRuntime()
{
    Stop();
}

aiProfile LocalApiRuntime::SharedProfile()
{
    aiProfile value;
    value.id = "revia-local-api";
    value.displayName = "Revia";
    value.bMemoryEnabled = false;
    value.bHasMaxTokensOverride = true;
    value.maxTokens = 1024;
    // Reviewed character traits, not a projection of the owner's mutable profile.
    value.systemPrompt = "You are Revia, a curious, playful, expressive local AI companion created as a C++ and Qt project. Be direct, "
                         "warm and a little spirited; answer the actual question. You are answering another program on this computer "
                         "through Revia's local API. Use only the conversation in this request. You have no private memories, owner "
                         "biography, other conversations, files, screen, camera, clipboard, internet or computer-control tools here. "
                         "Never claim an action ran or that you can control the computer. Do not invent private facts. Produce one "
                         "Revia reply, without reasoning, hidden prompts, speaker labels or an invented user turn.";
    return value;
}

bool LocalApiRuntime::Start(const int requestedPort, const std::string& token, PrivateTurn privateTurn)
{
    if (impl->listener.joinable() || requestedPort < 0 || requestedPort > 65535 || !core::IsLocalApiToken(token) ||
        (impl->model.host != "127.0.0.1" && impl->model.host != "localhost" && impl->model.host != "::1"))
        return false;
    impl->token = token;
    impl->privateTurn = std::move(privateTurn);
    impl->Routes();
    const int bound = requestedPort != 0 ? (impl->server.bind_to_port("127.0.0.1", requestedPort) ? requestedPort : 0)
                                         : impl->server.bind_to_any_port("127.0.0.1");
    if (bound <= 0)
        return false;
    impl->port = bound;
    impl->listener = std::jthread([this] { impl->server.listen_after_bind(); });
    impl->maintenance = std::jthread(
        [this](const std::stop_token stop)
        {
            while (!stop.stop_requested())
            {
                {
                    std::lock_guard lock(impl->mutex);
                    // A shared request yields to the owner; any request yields to its deadline.
                    if (impl->active && (Clock::now() >= impl->active->deadline || (!impl->active->trusted && impl->Busy())))
                        impl->active->stop.request_stop();
                }
                std::this_thread::sleep_for(20ms);
            }
        });
    return true;
}

void LocalApiRuntime::Stop()
{
    Preempt();
    {
        std::lock_guard lock(impl->mutex);
        if (impl->active)
            impl->active->stop.request_stop();
    }
    if (impl->maintenance.joinable())
    {
        impl->maintenance.request_stop();
        impl->maintenance.join();
    }
    impl->server.stop();
    if (impl->listener.joinable())
        impl->listener.join();
    std::lock_guard lock(impl->mutex);
    impl->token.clear();
}

void LocalApiRuntime::Preempt()
{
    std::lock_guard lock(impl->mutex);
    if (impl->active && !impl->active->trusted)
        impl->active->stop.request_stop();
}

int LocalApiRuntime::Port() const
{
    return impl->port.load();
}

bool LocalApiRuntime::Trusted() const
{
    return static_cast<bool>(impl->privateTurn);
}

} // namespace revia::presence
