#include "Mcp/mcpTransport.h"

#include "Actions/actionTypes.h"
#include "LLM/Cloud/cloudTransport.h"
#include "Process/ownedProcess.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>

namespace revia::mcp
{
namespace
{
using Clock = std::chrono::steady_clock;

class StdioTransport final : public McpTransport
{
  public:
    explicit StdioTransport(mcpServerSettings inputServer) : server(std::move(inputServer))
    {
    }

    ~StdioTransport() override
    {
        Close();
    }

    bool Open(std::string& error) override
    {
        process::OwnedProcessRequest request;
        request.executable = actions::Utf8ToPath(server.command);
        request.arguments = server.args;
        for (const auto& name : server.envAllowlist)
        {
            if (const char* value = std::getenv(name.c_str()))
                request.environment[name] = value;
        }
        // Unread output past one message and some slack stops the server.
        request.maxPendingOutputBytes = MaximumMessageBytes * 2;
        return child.Start(request, error);
    }

    bool Send(const std::string& message, std::stop_token, std::string& error) override
    {
        if (message.find('\n') != std::string::npos)
        {
            error = "A stdio message may not contain a newline.";
            return false;
        }
        return child.Write(message + "\n", error);
    }

    std::optional<std::string> Receive(const std::chrono::milliseconds wait) override
    {
        const auto deadline = Clock::now() + wait;
        for (;;)
        {
            if (auto line = NextLine())
                return line;
            if (overflowed)
                return std::nullopt;
            const auto now = Clock::now();
            if (now >= deadline || !child.Running())
                return std::nullopt;
            buffer += child.Read(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        }
    }

    bool Closed() const override
    {
        return overflowed || !child.Running();
    }

    std::string Diagnostics() const override
    {
        std::string reason = overflowed ? std::string("The server sent a message larger than Revia accepts.") : child.StopReason();
        const std::string errors = child.ErrorOutput();
        if (!errors.empty())
            reason += (reason.empty() ? "" : " ") + std::string("Its last error output: ") + errors.substr(errors.size() > 400 ? errors.size() - 400 : 0);
        return reason;
    }

    void Close() override
    {
        child.Terminate();
    }

  private:
    std::optional<std::string> NextLine()
    {
        for (;;)
        {
            const std::size_t end = buffer.find('\n');
            if (end == std::string::npos)
            {
                if (buffer.size() > MaximumMessageBytes)
                {
                    // One message too large to accept. The stream cannot be resynchronized
                    // safely, so the server is stopped.
                    overflowed = true;
                    buffer.clear();
                    child.Terminate();
                }
                return std::nullopt;
            }
            std::string line = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos)
                continue;
            if (line.size() > MaximumMessageBytes)
            {
                overflowed = true;
                child.Terminate();
                return std::nullopt;
            }
            return line;
        }
    }

    mcpServerSettings server;
    process::OwnedProcess child;
    std::string buffer;
    bool overflowed = false;
};

// Collects whole JSON-RPC messages from a response body that is either one JSON value
// or a server-sent event stream, fed in arbitrary chunks.
class ResponseDecoder
{
  public:
    // False when the body grew past what is accepted.
    bool Feed(std::string_view chunk)
    {
        total += chunk.size();
        if (total > MaximumMessageBytes * 2)
            return false;
        pending.append(chunk.data(), chunk.size());
        if (mode == Mode::Unknown)
        {
            const std::size_t first = pending.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return true;
            mode = pending[first] == '{' || pending[first] == '[' ? Mode::Json : Mode::Events;
        }
        if (mode == Mode::Events)
            DecodeEvents();
        return true;
    }

    // Called once the body has ended.
    void Finish(std::deque<std::string>& messages)
    {
        if (mode == Mode::Json && pending.find_first_not_of(" \t\r\n") != std::string::npos)
            decoded.push_back(pending);
        else if (mode == Mode::Events)
        {
            pending += "\n\n";
            DecodeEvents();
        }
        pending.clear();
        Take(messages);
    }

    void Take(std::deque<std::string>& messages)
    {
        while (!decoded.empty())
        {
            messages.push_back(std::move(decoded.front()));
            decoded.pop_front();
        }
    }

    [[nodiscard]] bool Contains(const nlohmann::json& id) const
    {
        for (const auto& message : decoded)
        {
            const auto parsed = nlohmann::json::parse(message, nullptr, false);
            if (parsed.is_object() && parsed.contains("id") && parsed["id"] == id && !parsed.contains("method"))
                return true;
        }
        return false;
    }

  private:
    enum class Mode
    {
        Unknown,
        Json,
        Events
    };

    void DecodeEvents()
    {
        for (;;)
        {
            std::size_t end = pending.find("\n\n");
            const std::size_t crlf = pending.find("\r\n\r\n");
            std::size_t skip = 2;
            if (crlf != std::string::npos && (end == std::string::npos || crlf < end))
            {
                end = crlf;
                skip = 4;
            }
            if (end == std::string::npos)
                return;
            const std::string event = pending.substr(0, end);
            pending.erase(0, end + skip);
            std::string data;
            std::size_t start = 0;
            while (start <= event.size())
            {
                std::size_t stop = event.find('\n', start);
                if (stop == std::string::npos)
                    stop = event.size();
                std::string line = event.substr(start, stop - start);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("data:", 0) == 0)
                {
                    std::string value = line.substr(5);
                    if (!value.empty() && value.front() == ' ')
                        value.erase(0, 1);
                    data += (data.empty() ? "" : "\n") + value;
                }
                start = stop + 1;
            }
            if (!data.empty())
                decoded.push_back(std::move(data));
        }
    }

    Mode mode = Mode::Unknown;
    std::string pending;
    std::deque<std::string> decoded;
    std::size_t total = 0;
};

class HttpTransport final : public McpTransport
{
  public:
    HttpTransport(mcpServerSettings inputServer, std::shared_ptr<llm::cloud::CloudTransport> inputHttp, std::string inputVersion)
        : server(std::move(inputServer)), http(std::move(inputHttp)), protocolVersion(std::move(inputVersion))
    {
    }

    bool Open(std::string& error) override
    {
        if (!http)
        {
            error = "No HTTP client is available for this server.";
            return false;
        }
        auto parsed = llm::cloud::ParseCloudEndpoint(server.url, error);
        if (!parsed)
            return false;
        endpoint = *parsed;
        open = true;
        return true;
    }

    bool Send(const std::string& message, const std::stop_token stopToken, std::string& error) override
    {
        if (!open)
        {
            error = "The server connection is closed.";
            return false;
        }
        llm::cloud::CloudHttpRequest request;
        request.endpoint = endpoint;
        request.path = endpoint.basePath.empty() ? "/" : endpoint.basePath;
        request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
        if (negotiated)
            request.headers.emplace_back("MCP-Protocol-Version", protocolVersion);
        if (!sessionId.empty())
            request.headers.emplace_back("Mcp-Session-Id", sessionId);
        request.body = message;
        request.timeoutSeconds = server.timeoutSeconds;
        request.responseHeaderNames = {"Mcp-Session-Id"};
        // A request is answered in this response; stop reading once its answer is in,
        // because a server may keep the stream open for later events.
        nlohmann::json awaited;
        if (const auto sent = nlohmann::json::parse(message, nullptr, false);
            sent.is_object() && sent.contains("method") && sent.contains("id"))
            awaited = sent["id"];
        ResponseDecoder decoder;
        bool tooLarge = false;
        const auto result = http->Post(
            request,
            [&](const int status, const std::string_view chunk)
            {
                if (status < 200 || status >= 300)
                    return true;
                if (!decoder.Feed(chunk))
                {
                    tooLarge = true;
                    return false;
                }
                return awaited.is_null() || !decoder.Contains(awaited);
            },
            stopToken);
        for (const auto& [name, value] : result.responseHeaders)
        {
            if (name == "Mcp-Session-Id" && !value.empty() && value.size() <= 256)
                sessionId = value;
        }
        if (tooLarge)
        {
            error = "The server's response was larger than Revia accepts.";
            return false;
        }
        if (!result.bReceived)
        {
            error = result.error.empty() ? "The server could not be reached." : result.error;
            return false;
        }
        if (result.status == 404 && !sessionId.empty())
        {
            // The server ended the session; the client must start over.
            open = false;
            error = "The server ended the session.";
            return false;
        }
        if (result.status < 200 || result.status >= 300)
        {
            error = "The server answered HTTP " + std::to_string(result.status) + ".";
            return false;
        }
        negotiated = true;
        std::lock_guard lock(mutex);
        decoder.Finish(inbox);
        return true;
    }

    std::optional<std::string> Receive(std::chrono::milliseconds) override
    {
        // Every answer arrived inside the POST that asked for it.
        std::lock_guard lock(mutex);
        if (inbox.empty())
            return std::nullopt;
        std::string message = std::move(inbox.front());
        inbox.pop_front();
        return message;
    }

    bool Closed() const override
    {
        return !open;
    }

    void Close() override
    {
        open = false;
    }

  private:
    mcpServerSettings server;
    std::shared_ptr<llm::cloud::CloudTransport> http;
    std::string protocolVersion;
    llm::cloud::CloudEndpoint endpoint;
    std::string sessionId;
    bool open = false;
    bool negotiated = false;
    std::mutex mutex;
    std::deque<std::string> inbox;
};
} // namespace

std::unique_ptr<McpTransport> MakeStdioTransport(const mcpServerSettings& server)
{
    return std::make_unique<StdioTransport>(server);
}

std::unique_ptr<McpTransport> MakeHttpTransport(
    const mcpServerSettings& server, std::shared_ptr<llm::cloud::CloudTransport> http, const std::string& protocolVersion)
{
    return std::make_unique<HttpTransport>(server, std::move(http), protocolVersion);
}

} // namespace revia::mcp
