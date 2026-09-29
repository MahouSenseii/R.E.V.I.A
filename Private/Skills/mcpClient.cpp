#include "Skills/mcpClient.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::skills
{

namespace
{
using json = nlohmann::json;
constexpr const char* OfferedProtocolVersion = "2025-06-18";

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}
} // namespace

McpClient::McpClient(std::string inputUrl, std::vector<std::pair<std::string, std::string>> inputHeaders)
    : url(std::move(inputUrl)), headers(std::move(inputHeaders))
{
    const std::size_t schemeEnd = url.find("://");
    scheme = schemeEnd == std::string::npos ? "http" : url.substr(0, schemeEnd);
    const std::string rest = schemeEnd == std::string::npos ? url : url.substr(schemeEnd + 3);
    const std::size_t slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && colon + 1 < authority.size() &&
        std::all_of(authority.begin() + static_cast<std::ptrdiff_t>(colon) + 1, authority.end(),
            [](const unsigned char c) { return std::isdigit(c) != 0; }))
    {
        host = authority.substr(0, colon);
        port = std::stoi(authority.substr(colon + 1));
    }
    else
    {
        host = authority;
        port = scheme == "https" ? 443 : 80;
    }
}

void McpClient::SetHttpsTransport(std::unique_ptr<llm::HttpsTransport> transport)
{
    if (transport) httpsTransport = std::move(transport);
}

bool McpClient::ExtractResult(
    const std::string& body, const std::string& contentType, const int id,
    std::string& outResultJson, std::string& outError, int* outErrorCode)
{
    // SSE: the answer is the last data line whose JSON carries our id.
    std::vector<std::string> candidates;
    if (contentType.find("text/event-stream") != std::string::npos)
    {
        std::istringstream stream(body);
        std::string line;
        std::string data;
        const auto flush = [&]
        {
            if (!data.empty()) candidates.push_back(data);
            data.clear();
        };
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty())
            {
                flush();
                continue;
            }
            if (line.rfind("data:", 0) == 0)
            {
                if (!data.empty()) data += '\n';
                data += Trim(line.substr(5));
            }
        }
        flush();
    }
    else
    {
        candidates.push_back(body);
    }
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
    {
        json message;
        try
        {
            message = json::parse(*it);
        }
        catch (const std::exception&)
        {
            continue;
        }
        if (!message.is_object() || !message.contains("id") ||
            !message["id"].is_number_integer() || message["id"].get<int>() != id)
        {
            continue;
        }
        if (message.contains("error"))
        {
            const json& error = message["error"];
            outError = error.is_object() ? error.value("message", "unknown error") : error.dump();
            if (outErrorCode != nullptr && error.is_object() && error.contains("code") &&
                error["code"].is_number_integer())
            {
                *outErrorCode = error["code"].get<int>();
            }
            return false;
        }
        outResultJson = message.contains("result") ? message["result"].dump() : "{}";
        return true;
    }
    outError = "The server's answer carried no response with the request's id.";
    return false;
}

McpClient::Reply McpClient::Post(const std::string& body)
{
    Reply reply;
    std::vector<std::pair<std::string, std::string>> requestHeaders = headers;
    requestHeaders.emplace_back("Accept", "application/json, text/event-stream");
    requestHeaders.emplace_back("MCP-Protocol-Version", OfferedProtocolVersion);
    if (!sessionId.empty()) requestHeaders.emplace_back("Mcp-Session-Id", sessionId);
    if (scheme == "https")
    {
        if (!httpsTransport) httpsTransport = llm::MakeSystemHttpsTransport();
        llm::HttpsRequest request;
        request.method = "POST";
        request.host = host;
        request.port = port;
        request.path = path;
        request.headers = requestHeaders;
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = body;
        request.timeoutSeconds = 60;
        const llm::HttpsResponse response = httpsTransport->Post(request, {});
        reply.completed = response.completed;
        reply.status = response.status;
        reply.body = response.body;
        reply.error = response.error;
        // The OS transport does not hand headers back; a session id on HTTPS is kept
        // only when the server puts it in the body, which the spec does not do, so a
        // remote server with sessions is used statelessly.
        reply.contentType = "application/json";
        return reply;
    }
    httplib::Client client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(60);
    httplib::Headers httpHeaders;
    for (const auto& [name, value] : requestHeaders) httpHeaders.emplace(name, value);
    const auto response = client.Post(path, httpHeaders, body, "application/json");
    if (!response)
    {
        reply.error = "No answer from " + host + ":" + std::to_string(port) + " (" +
            httplib::to_string(response.error()) + ").";
        return reply;
    }
    reply.completed = true;
    reply.status = response->status;
    reply.body = response->body;
    reply.contentType = response->get_header_value("Content-Type");
    reply.sessionId = response->get_header_value("Mcp-Session-Id");
    return reply;
}

bool McpClient::Call(const std::string& method, const std::string& paramsJson,
    std::string& outResultJson, std::string& outError, int* outErrorCode)
{
    const int id = nextId++;
    json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    try
    {
        request["params"] = paramsJson.empty() ? json::object() : json::parse(paramsJson);
    }
    catch (const std::exception& error)
    {
        outError = std::string("The call's arguments are not JSON: ") + error.what();
        return false;
    }
    const Reply reply = Post(request.dump());
    if (!reply.completed)
    {
        outError = reply.error;
        return false;
    }
    if (!reply.sessionId.empty()) sessionId = reply.sessionId;
    if (reply.status == 202 || (reply.status == 200 && Trim(reply.body).empty()))
    {
        outError = "The server accepted the request without answering it.";
        return false;
    }
    if (reply.status != 200)
    {
        outError = "HTTP " + std::to_string(reply.status) + " from " + url + ".";
        return false;
    }
    return ExtractResult(reply.body, reply.contentType, id, outResultJson, outError, outErrorCode);
}

void McpClient::Notify(const std::string& method)
{
    const json notification = {{"jsonrpc", "2.0"}, {"method", method}};
    (void)Post(notification.dump());
}

bool McpClient::Connect(std::string& outError)
{
    const json params = {
        {"protocolVersion", OfferedProtocolVersion},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "Revia"}, {"version", "1.0"}}}};
    std::string result;
    int code = 0;
    if (Call("initialize", params.dump(), result, outError, &code))
    {
        try
        {
            const json parsed = json::parse(result);
            protocolVersion = parsed.value("protocolVersion", "");
            const json info = parsed.value("serverInfo", json::object());
            serverName = info.value("name", "");
        }
        catch (const std::exception&)
        {
        }
        Notify("notifications/initialized");
        outError.clear();
        return true;
    }
    if (code == -32601)
    {
        // A server past the handshake: nothing to initialize, straight to the tools.
        protocolVersion = "2026-07-28";
        outError.clear();
        return true;
    }
    return false;
}

std::vector<McpToolInfo> McpClient::ListTools(std::string& outError)
{
    std::vector<McpToolInfo> tools;
    std::string result;
    if (!Call("tools/list", "{}", result, outError)) return tools;
    try
    {
        const json parsed = json::parse(result);
        for (const json& item : parsed.value("tools", json::array()))
        {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) continue;
            McpToolInfo tool;
            tool.name = item["name"].get<std::string>();
            tool.description = item.value("description", "");
            tool.inputSchema = item.contains("inputSchema") ? item["inputSchema"].dump() : "{}";
            tools.push_back(std::move(tool));
        }
    }
    catch (const std::exception& error)
    {
        outError = std::string("The tool list could not be read: ") + error.what();
        tools.clear();
    }
    return tools;
}

McpToolResult McpClient::CallTool(const std::string& name, const std::string& argumentsJson)
{
    McpToolResult result;
    json arguments = json::object();
    if (!Trim(argumentsJson).empty())
    {
        try
        {
            arguments = json::parse(argumentsJson);
        }
        catch (const std::exception& error)
        {
            result.reason = std::string("The tool's arguments are not JSON: ") + error.what();
            return result;
        }
        if (!arguments.is_object())
        {
            result.reason = "The tool's arguments must be a JSON object.";
            return result;
        }
    }
    const json params = {{"name", name}, {"arguments", arguments}};
    std::string resultJson;
    if (!Call("tools/call", params.dump(), resultJson, result.reason)) return result;
    try
    {
        const json parsed = json::parse(resultJson);
        result.toolError = parsed.value("isError", false);
        std::string text;
        for (const json& block : parsed.value("content", json::array()))
        {
            if (!block.is_object()) continue;
            const std::string type = block.value("type", "");
            if (type == "text" && block.contains("text") && block["text"].is_string())
            {
                if (!text.empty()) text += '\n';
                text += block["text"].get<std::string>();
            }
            else if (!type.empty())
            {
                if (!text.empty()) text += '\n';
                text += "[" + type + " content omitted]";
            }
        }
        if (text.empty() && parsed.contains("structuredContent"))
        {
            text = parsed["structuredContent"].dump();
        }
        result.text = text;
        result.succeeded = true;
    }
    catch (const std::exception& error)
    {
        result.reason = std::string("The tool's answer could not be read: ") + error.what();
    }
    return result;
}

} // namespace revia::skills
