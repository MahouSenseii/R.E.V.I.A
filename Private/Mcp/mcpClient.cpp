#include "Mcp/mcpClient.h"

#include "Core/utf8.h"

#include <algorithm>
#include <optional>

namespace revia::mcp
{
namespace
{
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::size_t MaximumToolPages = 16;
constexpr std::size_t MaximumTools = 256;
// Messages that are not JSON at all. A few are tolerated, as a server's stray log line;
// a stream of them means the other end is not speaking the protocol.
constexpr int MaximumGarbageMessages = 32;
constexpr std::chrono::milliseconds PollSlice{50};

bool SupportedVersion(const std::string& version)
{
    return version == "2025-06-18" || version == "2025-03-26" || version == "2024-11-05";
}

std::string Bounded(const std::string& text, const std::size_t bytes)
{
    const std::string clean = utf8::Sanitize(text);
    return clean.size() > bytes ? utf8::Prefix(clean, bytes) : clean;
}

std::string ErrorText(const Json& error)
{
    std::string message = error.is_object() && error.contains("message") && error["message"].is_string()
                              ? Bounded(error["message"].get<std::string>(), 300)
                              : std::string("no message");
    if (error.is_object() && error.contains("code") && error["code"].is_number_integer())
        message += " (code " + std::to_string(error["code"].get<long long>()) + ")";
    return "The server returned an error: " + message;
}
} // namespace

McpClient::McpClient(std::unique_ptr<McpTransport> inputTransport, const std::chrono::milliseconds inputTimeout)
    : transport(std::move(inputTransport)), timeout(inputTimeout)
{
}

McpClient::~McpClient()
{
    Close();
}

bool McpClient::Initialize(const std::stop_token stopToken, std::string& error)
{
    {
        std::lock_guard lock(mutex);
        if (!transport || !transport->Open(error))
        {
            if (error.empty())
                error = "The server transport could not be opened.";
            return false;
        }
    }
    const Json params = {{"protocolVersion", ProtocolVersion}, {"capabilities", Json::object()},
        {"clientInfo", {{"name", "Revia"}, {"title", "R.E.V.I.A"}, {"version", "0.2"}}}};
    Json result;
    if (!Request("initialize", params, stopToken, result, error))
        return false;
    const std::string version = result.is_object() ? result.value("protocolVersion", std::string{}) : std::string{};
    if (!SupportedVersion(version))
    {
        error = "The server speaks protocol revision \"" + Bounded(version, 32) + "\", which Revia does not.";
        Close();
        return false;
    }
    std::lock_guard lock(mutex);
    negotiatedVersion = version;
    if (result.contains("serverInfo") && result["serverInfo"].is_object() && result["serverInfo"].contains("name") &&
        result["serverInfo"]["name"].is_string())
        serverName = Bounded(result["serverInfo"]["name"].get<std::string>(), 80);
    Notify("notifications/initialized", Json());
    connected = true;
    return true;
}

bool McpClient::ListTools(std::vector<McpToolInfo>& tools, const std::stop_token stopToken, std::string& error)
{
    tools.clear();
    std::string cursor;
    for (std::size_t page = 0; page < MaximumToolPages; ++page)
    {
        Json params = Json::object();
        if (!cursor.empty())
            params["cursor"] = cursor;
        // Cleared before the request: a change announced while the list is read must
        // still be seen afterwards.
        {
            std::lock_guard lock(mutex);
            if (page == 0)
                toolsChanged = false;
        }
        Json result;
        if (!Request("tools/list", params, stopToken, result, error))
            return false;
        if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array())
        {
            error = "The server's tool list is malformed.";
            return false;
        }
        for (const auto& entry : result["tools"])
        {
            if (tools.size() >= MaximumTools)
                break;
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
                continue;
            McpToolInfo tool;
            tool.name = entry["name"].get<std::string>();
            if (tool.name.empty() || tool.name.size() > 128 || !utf8::IsValid(tool.name))
                continue;
            if (entry.contains("title") && entry["title"].is_string())
                tool.title = Bounded(entry["title"].get<std::string>(), 120);
            if (entry.contains("description") && entry["description"].is_string())
                tool.description = Bounded(entry["description"].get<std::string>(), 2048);
            if (entry.contains("inputSchema"))
                tool.inputSchema = entry["inputSchema"];
            tools.push_back(std::move(tool));
        }
        if (!result.contains("nextCursor") || !result["nextCursor"].is_string() || result["nextCursor"].get<std::string>().empty())
            return true;
        cursor = result["nextCursor"].get<std::string>();
        if (cursor.size() > 1024)
        {
            error = "The server's tool list cursor is too long.";
            return false;
        }
    }
    // More pages than this is not a tool list a person reviewed; what was read is kept.
    return true;
}

McpCallResult McpClient::CallTool(const std::string& name, const Json& arguments, const std::stop_token stopToken)
{
    McpCallResult call;
    Json result;
    std::string error;
    if (!Request("tools/call", {{"name", name}, {"arguments", arguments}}, stopToken, result, error, &call.timedOut, &call.cancelled,
            &call.resultBytes))
    {
        call.error = error;
        return call;
    }
    call.answered = true;
    call.toolError = result.is_object() && result.value("isError", false) == true;
    call.text = DescribeToolContent(result, MaximumResultTextBytes);
    return call;
}

void McpClient::Poll()
{
    std::lock_guard lock(mutex);
    if (!transport)
        return;
    for (int index = 0; index < 64; ++index)
    {
        const auto message = transport->Receive(std::chrono::milliseconds(0));
        if (!message)
            break;
        const Json parsed = Json::parse(*message, nullptr, false);
        if (parsed.is_array())
        {
            for (const auto& entry : parsed)
                Handle(entry);
        }
        else
        {
            Handle(parsed);
        }
    }
    if (transport->Closed())
        connected = false;
}

bool McpClient::ToolsChanged() const
{
    std::lock_guard lock(mutex);
    return toolsChanged;
}

bool McpClient::Connected() const
{
    std::lock_guard lock(mutex);
    return connected && transport && !transport->Closed();
}

std::string McpClient::ServerName() const
{
    std::lock_guard lock(mutex);
    return serverName;
}

std::string McpClient::NegotiatedVersion() const
{
    std::lock_guard lock(mutex);
    return negotiatedVersion;
}

std::string McpClient::Diagnostics() const
{
    std::lock_guard lock(mutex);
    return transport ? transport->Diagnostics() : std::string{};
}

void McpClient::Close()
{
    std::lock_guard lock(mutex);
    connected = false;
    if (transport)
        transport->Close();
}

bool McpClient::Request(const std::string& method, const Json& params, const std::stop_token stopToken, Json& result, std::string& error,
    bool* timedOut, bool* cancelled, std::size_t* bytes)
{
    std::lock_guard lock(mutex);
    if (!transport || transport->Closed())
    {
        error = "The server is not running.";
        return false;
    }
    const std::int64_t id = nextId++;
    Json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null())
        request["params"] = params;
    if (!transport->Send(request.dump(-1, ' ', false, Json::error_handler_t::replace), stopToken, error))
    {
        if (error.empty())
            error = "The request could not be sent to the server.";
        return false;
    }
    const auto deadline = Clock::now() + timeout;
    int garbage = 0;
    const auto giveUp = [&](const std::string& reason, bool* flag)
    {
        if (flag)
            *flag = true;
        error = reason;
        // The server may stop its work; a late answer is dropped by Handle.
        Notify("notifications/cancelled", {{"requestId", id}, {"reason", reason}});
        return false;
    };
    for (;;)
    {
        if (stopToken.stop_requested())
            return giveUp("The request was cancelled.", cancelled);
        const auto now = Clock::now();
        if (now >= deadline)
            return giveUp("The server did not answer within its timeout.", timedOut);
        const auto wait = std::min(PollSlice, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1));
        const auto message = transport->Receive(wait);
        if (!message)
        {
            if (transport->Closed())
            {
                connected = false;
                error = "The server closed the connection.";
                const std::string detail = transport->Diagnostics();
                if (!detail.empty())
                    error += " " + detail;
                return false;
            }
            continue;
        }
        const Json parsed = Json::parse(*message, nullptr, false);
        if (parsed.is_discarded())
        {
            if (++garbage > MaximumGarbageMessages)
            {
                error = "The server is not speaking JSON-RPC.";
                return false;
            }
            continue;
        }
        std::vector<Json> entries;
        if (parsed.is_array())
            entries.assign(parsed.begin(), parsed.end());
        else
            entries.push_back(parsed);
        for (const auto& entry : entries)
        {
            const bool answer = entry.is_object() && !entry.contains("method") && entry.contains("id") && entry["id"] == id;
            if (!answer)
            {
                Handle(entry);
                continue;
            }
            if (bytes)
                *bytes = message->size();
            if (entry.contains("error"))
            {
                error = ErrorText(entry["error"]);
                return false;
            }
            if (!entry.contains("result"))
            {
                error = "The server's answer has no result.";
                return false;
            }
            result = entry["result"];
            return true;
        }
    }
}

void McpClient::Notify(const std::string& method, const Json& params)
{
    Json message = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null())
        message["params"] = params;
    std::string ignored;
    if (transport)
        static_cast<void>(transport->Send(message.dump(-1, ' ', false, Json::error_handler_t::replace), {}, ignored));
}

void McpClient::Handle(const Json& message)
{
    if (!message.is_object() || !message.contains("method") || !message["method"].is_string())
        return;
    const std::string method = message["method"].get<std::string>();
    if (message.contains("id"))
    {
        // A request from the server. Only ping is answered; sampling, elicitation and
        // roots are capabilities Revia never declared, and never grants.
        Json reply = {{"jsonrpc", "2.0"}, {"id", message["id"]}};
        if (method == "ping")
            reply["result"] = Json::object();
        else
            reply["error"] = {{"code", -32601}, {"message", "Method not found"}};
        std::string ignored;
        static_cast<void>(transport->Send(reply.dump(-1, ' ', false, Json::error_handler_t::replace), {}, ignored));
        return;
    }
    if (method == "notifications/tools/list_changed")
        toolsChanged = true;
}

std::string DescribeToolContent(const Json& result, const std::size_t maximumBytes)
{
    std::string text;
    const auto add = [&text](const std::string& part)
    {
        if (part.empty())
            return;
        text += (text.empty() ? "" : "\n") + part;
    };
    const Json content = result.is_object() && result.contains("content") && result["content"].is_array() ? result["content"] : Json::array();
    for (const auto& item : content)
    {
        if (!item.is_object())
            continue;
        const std::string type = item.value("type", std::string{});
        const auto field = [&item](const char* key)
        { return item.contains(key) && item[key].is_string() ? utf8::Sanitize(item[key].get<std::string>()) : std::string{}; };
        if (type == "text")
            add(field("text"));
        else if (type == "image" || type == "audio")
            add("[" + std::string(type == "image" ? "An image" : "Audio") + " (" + Bounded(field("mimeType"), 64) +
                ") was returned and not shown.]");
        else if (type == "resource_link")
            add("Resource link: " + Bounded(field("name"), 200) + " <" + Bounded(field("uri"), 1024) + ">");
        else if (type == "resource" && item.contains("resource") && item["resource"].is_object())
        {
            const Json& resource = item["resource"];
            const std::string uri = resource.contains("uri") && resource["uri"].is_string() ? Bounded(resource["uri"].get<std::string>(), 1024)
                                                                                           : std::string{};
            if (resource.contains("text") && resource["text"].is_string())
                add("Resource <" + uri + ">:\n" + utf8::Sanitize(resource["text"].get<std::string>()));
            else
                add("[A binary resource <" + uri + "> was returned and not shown.]");
        }
        else
            add("[Content of an unsupported type was returned and not shown.]");
        if (text.size() > maximumBytes)
            break;
    }
    if (text.empty() && result.is_object() && result.contains("structuredContent"))
        text = result["structuredContent"].dump(-1, ' ', false, Json::error_handler_t::replace);
    const std::string truncated = "\n[Result truncated by Revia.]";
    if (text.size() > maximumBytes)
        text = utf8::Prefix(text, maximumBytes > truncated.size() ? maximumBytes - truncated.size() : 0) + truncated;
    return text;
}

} // namespace revia::mcp
