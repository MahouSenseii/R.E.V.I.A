#include "Internet/visibleBrowserClient.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace revia::actions::internet
{

std::uint64_t VisibleBrowserCancellation::BeginRequest(
    const int port,
    const std::string& token)
{
    std::lock_guard lock(mutex);
    if (nextRequestId == 0) nextRequestId = 1;
    activeRequestId = nextRequestId++;
    cancelledRequestId = 0;
    activePort = port;
    activeToken = token;
    return activeRequestId;
}

void VisibleBrowserCancellation::EndRequest(const std::uint64_t requestId)
{
    std::lock_guard lock(mutex);
    if (activeRequestId != requestId) return;
    activeRequestId = 0;
    cancelledRequestId = 0;
    activePort = 0;
    activeToken.clear();
}

bool VisibleBrowserCancellation::IsCancelled(const std::uint64_t requestId) const
{
    std::lock_guard lock(mutex);
    return requestId != 0 && cancelledRequestId == requestId;
}

void VisibleBrowserCancellation::CancelActive()
{
    int port = 0;
    std::string token;
    {
        std::lock_guard lock(mutex);
        if (activeRequestId == 0) return;
        cancelledRequestId = activeRequestId;
        port = activePort;
        token = activeToken;
    }
    if (port <= 0 || token.empty()) return;
    // Do not hold the state mutex while WinHTTP waits. The executor must be able to
    // observe cancellation immediately and unwind its registration.
    VisibleBrowserClient(port, std::move(token)).RequestShutdown();
}

VisibleBrowserClient::VisibleBrowserClient(const int inputPort, std::string inputToken)
    : port(inputPort), token(std::move(inputToken))
{
}

bool VisibleBrowserClient::WaitUntilReady(
    const int timeoutMs,
    std::string& outError,
    const std::function<bool()>& cancelled) const
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(1, timeoutMs));
    std::string lastError = "The visible browser worker did not become ready.";
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cancelled && cancelled())
        {
            outError = "The visible browser startup was cancelled.";
            return false;
        }
        int status = 0;
        std::string body;
        std::string error;
        if (Request("GET", "/health", {}, 1000, 4096, status, body, error) && status == 200)
        {
            try
            {
                const nlohmann::json response = nlohmann::json::parse(body);
                if (response.value("ready", false))
                {
                    outError.clear();
                    return true;
                }
                lastError = response.value("message", lastError);
            }
            catch (...)
            {
                lastError = "The visible browser worker returned an invalid health response.";
            }
        }
        else if (!error.empty())
        {
            lastError = error;
        }
        if (cancelled && cancelled())
        {
            outError = "The visible browser startup was cancelled.";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    outError = lastError;
    return false;
}

ActionResult VisibleBrowserClient::Search(
    const std::string& query,
    const int maxResults,
    const std::size_t maxResponseBytes,
    const int timeoutMs,
    const int stepDelayMs) const
{
    const nlohmann::json request = {
        {"query", query},
        {"max_results", std::clamp(maxResults, 1, 5)},
        {"max_response_bytes", std::clamp<std::size_t>(
            maxResponseBytes, 4096U, 2U * 1024U * 1024U)},
        {"timeout_ms", std::clamp(timeoutMs, 5000, 120000)},
        {"step_delay_ms", std::clamp(stepDelayMs, 0, 3000)}};
    int status = 0;
    std::string body;
    std::string error;
    if (!Request(
            "POST", "/search", request.dump(), timeoutMs + 5000,
            maxResponseBytes + 64U * 1024U, status, body, error))
    {
        ActionResult result;
        result.attempted = true;
        result.message = error.empty()
            ? "The visible browser worker did not return a response."
            : error;
        return result;
    }
    return ParseSearchResponse(body, status, maxResponseBytes, maxResults);
}

void VisibleBrowserClient::RequestShutdown() const
{
    int status = 0;
    std::string body;
    std::string error;
    static_cast<void>(Request("POST", "/shutdown", "{}", 1000, 4096, status, body, error));
}

ActionResult VisibleBrowserClient::ParseSearchResponse(
    const std::string& body,
    const int statusCode,
    const std::size_t maxResponseBytes,
    const int maxResults)
{
    ActionResult result;
    result.attempted = true;
    try
    {
        const nlohmann::json response = nlohmann::json::parse(body);
        result.message = response.value("message", "The visible browser returned no status message.");
        if (response.contains("content") && response["content"].is_string())
        {
            result.content = response["content"].get<std::string>();
            if (result.content.size() > maxResponseBytes)
            {
                result.content.resize(maxResponseBytes);
            }
        }
        if (response.contains("entries") && response["entries"].is_array())
        {
            const int boundedResults = std::clamp(maxResults, 1, 5);
            for (const auto& entry : response["entries"])
            {
                if (static_cast<int>(result.entries.size()) >= boundedResults) break;
                if (!entry.is_string()) continue;
                const std::string url = entry.get<std::string>();
                if (url.size() <= 2048 && url.rfind("https://", 0) == 0)
                    result.entries.push_back(url);
            }
        }
        result.succeeded = statusCode == 200 && response.value("succeeded", false) &&
            !result.content.empty() && !result.entries.empty();
        if (statusCode != 200 && result.message.empty())
            result.message = "The visible browser service returned HTTP " +
                std::to_string(statusCode) + ".";
        return result;
    }
    catch (const std::exception& error)
    {
        result.message = std::string("The visible browser response was invalid JSON: ") +
            error.what();
        return result;
    }
}

bool VisibleBrowserClient::Request(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const int timeoutMs,
    const std::size_t maxBytes,
    int& outStatus,
    std::string& outBody,
    std::string& outError) const
{
    outStatus = 0;
    outBody.clear();
    outError.clear();
#ifndef _WIN32
    (void)method;
    (void)path;
    (void)body;
    (void)timeoutMs;
    (void)maxBytes;
    outError = "The visible browser client is currently supported on Windows only.";
    return false;
#else
    if (port < 1 || port > 65535 || token.empty())
    {
        outError = "The visible browser loopback endpoint is not configured.";
        return false;
    }

    const std::wstring wideMethod(method.begin(), method.end());
    const std::wstring widePath(path.begin(), path.end());
    HINTERNET session = WinHttpOpen(
        L"Revia/0.2 visible-browser-client", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        outError = "Windows could not initialize the visible browser loopback client.";
        return false;
    }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET connection = WinHttpConnect(
        session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    HINTERNET request = connection != nullptr
        ? WinHttpOpenRequest(
            connection, wideMethod.c_str(), widePath.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
        : nullptr;
    const std::wstring headers = L"Authorization: Bearer " +
        std::wstring(token.begin(), token.end()) +
        L"\r\nContent-Type: application/json\r\nAccept: application/json\r\n";
    const bool sent = request != nullptr && WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(-1L),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (!sent)
    {
        outError = "The visible browser loopback request failed.";
        if (request != nullptr) WinHttpCloseHandle(request);
        if (connection != nullptr) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    WinHttpQueryHeaders(
        request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX);
    outStatus = static_cast<int>(status);
    while (outBody.size() <= maxBytes)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            outError = "The visible browser response could not be read.";
            break;
        }
        if (available == 0) break;
        const std::size_t remaining = maxBytes + 1U - outBody.size();
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(available, remaining));
        std::string chunk(wanted, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), wanted, &read))
        {
            outError = "The visible browser response body could not be read.";
            break;
        }
        chunk.resize(read);
        outBody += chunk;
        if (outBody.size() > maxBytes)
        {
            outError = "The visible browser response exceeded its byte limit.";
            break;
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return outError.empty();
#endif
}

} // namespace revia::actions::internet
