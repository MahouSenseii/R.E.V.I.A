#pragma once

#include "LLM/httpsTransport.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace revia::skills
{

// One MCP server, spoken to over Streamable HTTP.
//
// JSON-RPC 2.0, one request per POST, the answer as a JSON body or as a text/event-stream
// whose last data line carries it. The client keeps the session id a server hands out
// and offers the 2025-06-18 handshake; a server that has dropped `initialize` (the
// 2026-07-28 revision) answers "method not found" and is simply used without it. It
// lists tools and calls tools, and nothing else: no resources, no prompts, no
// sampling, no elicitation -- a server cannot ask Revia for anything through this.
// Plain HTTP goes through the loopback client; HTTPS through the OS's TLS.
struct McpToolInfo
{
    std::string name;
    std::string description;
    // The input schema as JSON text.
    std::string inputSchema;
};

struct McpToolResult
{
    bool succeeded = false;
    // The server flagged the call as an error (isError), as opposed to a transport
    // failure; the text says what it said.
    bool toolError = false;
    std::string text;
    std::string reason;
};

class McpClient
{
public:
    McpClient(std::string url, std::vector<std::pair<std::string, std::string>> headers = {});

    // The handshake, when the server still has one. False only when the server could
    // not be reached or answered with a real error.
    [[nodiscard]] bool Connect(std::string& outError);
    [[nodiscard]] std::vector<McpToolInfo> ListTools(std::string& outError);
    [[nodiscard]] McpToolResult CallTool(const std::string& name, const std::string& argumentsJson);

    [[nodiscard]] const std::string& Url() const { return url; }
    [[nodiscard]] const std::string& ServerName() const { return serverName; }
    [[nodiscard]] const std::string& ProtocolVersion() const { return protocolVersion; }
    // A test's transport for an https endpoint.
    void SetHttpsTransport(std::unique_ptr<llm::HttpsTransport> transport);

    // A response body as the server sent it -- JSON or SSE -- to the JSON-RPC result
    // for `id`, or an error message. Public for the tests.
    [[nodiscard]] static bool ExtractResult(
        const std::string& body, const std::string& contentType, int id,
        std::string& outResultJson, std::string& outError, int* outErrorCode = nullptr);

private:
    struct Reply
    {
        bool completed = false;
        int status = 0;
        std::string contentType;
        std::string body;
        std::string sessionId;
        std::string error;
    };
    [[nodiscard]] Reply Post(const std::string& body);
    [[nodiscard]] bool Call(const std::string& method, const std::string& paramsJson,
        std::string& outResultJson, std::string& outError, int* outErrorCode = nullptr);
    void Notify(const std::string& method);

    std::string url;
    std::string scheme;
    std::string host;
    int port = 80;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string sessionId;
    std::string serverName;
    std::string protocolVersion;
    int nextId = 1;
    std::unique_ptr<llm::HttpsTransport> httpsTransport;
};

} // namespace revia::skills
