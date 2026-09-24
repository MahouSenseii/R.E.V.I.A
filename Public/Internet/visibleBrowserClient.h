#pragma once

#include "Actions/actionTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace revia::actions::internet
{

// A small cancellation bridge shared by ActionRuntime and its browser executor. It does
// not expose a general HTTP surface: cancellation can only send the authenticated
// shutdown request to the currently active project-owned browser worker.
class VisibleBrowserCancellation
{
public:
    [[nodiscard]] std::uint64_t BeginRequest(int port, const std::string& token);
    void EndRequest(std::uint64_t requestId);
    [[nodiscard]] bool IsCancelled(std::uint64_t requestId) const;
    void CancelActive();

private:
    mutable std::mutex mutex;
    std::uint64_t nextRequestId = 1;
    std::uint64_t activeRequestId = 0;
    std::uint64_t cancelledRequestId = 0;
    int activePort = 0;
    std::string activeToken;
};

// Narrow loopback client for the project-owned worker. It has no general URL method:
// callers may ask only for health, one bounded search, or a graceful shutdown.
class VisibleBrowserClient
{
public:
    VisibleBrowserClient(int port, std::string token);

    [[nodiscard]] bool WaitUntilReady(
        int timeoutMs,
        std::string& outError,
        const std::function<bool()>& cancelled = {}) const;
    [[nodiscard]] ActionResult Search(
        const std::string& query,
        int maxResults,
        std::size_t maxResponseBytes,
        int timeoutMs,
        int stepDelayMs) const;
    void RequestShutdown() const;

    // Public for deterministic tests; performs no network access.
    [[nodiscard]] static ActionResult ParseSearchResponse(
        const std::string& body,
        int statusCode,
        std::size_t maxResponseBytes,
        int maxResults);

private:
    [[nodiscard]] bool Request(
        const std::string& method,
        const std::string& path,
        const std::string& body,
        int timeoutMs,
        std::size_t maxBytes,
        int& outStatus,
        std::string& outBody,
        std::string& outError) const;

    int port = 0;
    std::string token;
};

} // namespace revia::actions::internet
