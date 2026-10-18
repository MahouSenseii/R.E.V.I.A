#pragma once

#include "Mcp/mcpTransport.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::mcp
{

// The protocol revision Revia asks for. A server that answers with another revision
// Revia does not speak is disconnected.
inline constexpr const char* ProtocolVersion = "2025-06-18";

struct McpToolInfo
{
    std::string name;
    std::string title;
    std::string description;
    nlohmann::json inputSchema;
};

struct McpCallResult
{
    // The server answered; `toolError` says whether the tool itself reported failure.
    bool answered = false;
    bool toolError = false;
    // Text content, resource links listed by name and address, images and audio noted
    // but never decoded. Bounded; still untrusted.
    std::string text;
    // Size of the raw result as the server sent it, for the audit.
    std::size_t resultBytes = 0;
    // Why there is no answer: a timeout, a cancellation, a protocol error.
    std::string error;
    bool timedOut = false;
    bool cancelled = false;
};

// A Model Context Protocol client over one transport. It offers no capabilities of its
// own: a server's requests to sample Revia's model, elicit input from the person or list
// roots are answered "method not found", so a server can never drive her model or reach
// the person through her. Calls are serialized; each waits up to the configured timeout,
// and a timeout or a stop sends notifications/cancelled for that request.
class McpClient
{
  public:
    McpClient(std::unique_ptr<McpTransport> transport, std::chrono::milliseconds timeout);
    ~McpClient();
    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    // Opens the transport, runs the initialize handshake and sends notifications/initialized.
    [[nodiscard]] bool Initialize(std::stop_token stopToken, std::string& error);
    // Every page of tools/list, bounded to 16 pages and 256 tools.
    [[nodiscard]] bool ListTools(std::vector<McpToolInfo>& tools, std::stop_token stopToken, std::string& error);
    [[nodiscard]] McpCallResult CallTool(const std::string& name, const nlohmann::json& arguments, std::stop_token stopToken);
    // Handles whatever the server sent since the last call, without waiting.
    void Poll();
    // A notifications/tools/list_changed arrived since the last ListTools.
    [[nodiscard]] bool ToolsChanged() const;
    [[nodiscard]] bool Connected() const;
    [[nodiscard]] std::string ServerName() const;
    [[nodiscard]] std::string NegotiatedVersion() const;
    [[nodiscard]] std::string Diagnostics() const;
    void Close();

    // Results larger than this are cut before they reach the tool loop's own bound.
    static constexpr std::size_t MaximumResultTextBytes = 16384;

  private:
    // Sends one request and waits for its answer, handling everything else the server
    // sends meanwhile. False with `error` set on a JSON-RPC error, timeout or stop.
    bool Request(const std::string& method, const nlohmann::json& params, std::stop_token stopToken, nlohmann::json& result,
        std::string& error, bool* timedOut = nullptr, bool* cancelled = nullptr, std::size_t* bytes = nullptr);
    void Notify(const std::string& method, const nlohmann::json& params);
    // A message that is not the awaited answer: a notification, a server request, or a
    // late answer to a request already given up on.
    void Handle(const nlohmann::json& message);

    mutable std::mutex mutex;
    std::unique_ptr<McpTransport> transport;
    std::chrono::milliseconds timeout;
    std::int64_t nextId = 1;
    bool connected = false;
    bool toolsChanged = false;
    std::string serverName;
    std::string negotiatedVersion;
};

// Joins a tools/call result's content into text the tool loop can bound and label.
[[nodiscard]] std::string DescribeToolContent(const nlohmann::json& result, std::size_t maximumBytes);

} // namespace revia::mcp
