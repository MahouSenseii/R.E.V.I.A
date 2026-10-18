#pragma once

#include "Mcp/mcpSettings.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace revia::llm::cloud
{
class CloudTransport;
}

namespace revia::mcp
{

// A JSON-RPC message larger than this is refused; no tool list or result needs more.
inline constexpr std::size_t MaximumMessageBytes = 4U * 1024U * 1024U;

// How the client reaches one server: whole JSON-RPC messages out, whole messages in.
// The protocol layer never knows whether a child process or an HTTP endpoint is behind
// it, which is also what lets tests drive it in memory.
class McpTransport
{
  public:
    virtual ~McpTransport() = default;
    [[nodiscard]] virtual bool Open(std::string& error) = 0;
    // One serialized JSON-RPC message.
    [[nodiscard]] virtual bool Send(const std::string& message, std::stop_token stopToken, std::string& error) = 0;
    // The next complete message, or nothing when none arrives within `wait`.
    [[nodiscard]] virtual std::optional<std::string> Receive(std::chrono::milliseconds wait) = 0;
    [[nodiscard]] virtual bool Closed() const = 0;
    // A short diagnostic for status lines, such as the newest standard error.
    [[nodiscard]] virtual std::string Diagnostics() const
    {
        return {};
    }
    virtual void Close() = 0;
};

// Starts `server.command` through the owned-process layer: no shell, a clean
// environment holding only the allowlisted variables, a kill-on-close process tree and
// bounded output. Messages are newline-delimited JSON on standard input and output.
[[nodiscard]] std::unique_ptr<McpTransport> MakeStdioTransport(const mcpServerSettings& server);

// Streamable HTTP: each message is a POST to `server.url`; the answer is JSON or a
// server-sent event stream. `http` carries the bytes; WinHTTP in production, so the
// cloud backend's HTTPS rules and redirect refusal apply here too.
[[nodiscard]] std::unique_ptr<McpTransport> MakeHttpTransport(
    const mcpServerSettings& server, std::shared_ptr<llm::cloud::CloudTransport> http, const std::string& protocolVersion);

} // namespace revia::mcp
