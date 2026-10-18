#include "LLM/Cloud/cloudTransport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace revia::llm::cloud
{

namespace
{
std::string Lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A reply larger than this is not a chat answer; stop reading rather than grow forever.
constexpr std::size_t MaximumResponseBytes = 8U * 1024U * 1024U;
} // namespace

std::optional<CloudEndpoint> ParseCloudEndpoint(const std::string& baseUrl, std::string& outError)
{
    const std::string lowered = Lowered(baseUrl);
    CloudEndpoint endpoint;
    std::size_t rest = 0;
    if (lowered.rfind("https://", 0) == 0)
    {
        endpoint.secure = true;
        endpoint.port = 443;
        rest = 8;
    }
    else if (lowered.rfind("http://", 0) == 0)
    {
        endpoint.secure = false;
        endpoint.port = 80;
        rest = 7;
    }
    else
    {
        outError = "The cloud base URL must start with https://.";
        return std::nullopt;
    }
    const std::string remainder = baseUrl.substr(rest);
    if (remainder.find_first_of("?#@[]\\ \t\r\n") != std::string::npos)
    {
        // A user:password@ part would put a credential in the URL, and a query or a
        // fragment has no meaning for a base URL. Neither is accepted.
        outError = "The cloud base URL may hold only a host, an optional port and a path.";
        return std::nullopt;
    }
    const std::size_t slash = remainder.find('/');
    const std::string authority = remainder.substr(0, slash);
    std::string path = slash == std::string::npos ? std::string{} : remainder.substr(slash);
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    const std::size_t colon = authority.find(':');
    endpoint.host = Lowered(authority.substr(0, colon));
    if (colon != std::string::npos)
    {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](const unsigned char c) { return std::isdigit(c); }))
        {
            outError = "The cloud base URL has an invalid port.";
            return std::nullopt;
        }
        endpoint.port = std::stoi(port);
        if (endpoint.port < 1 || endpoint.port > 65535)
        {
            outError = "The cloud base URL has an invalid port.";
            return std::nullopt;
        }
    }
    if (endpoint.host.empty())
    {
        outError = "The cloud base URL has no host.";
        return std::nullopt;
    }
    if (!endpoint.secure && endpoint.host != "127.0.0.1")
    {
        // The only plain-HTTP peer allowed is a test server on this machine. "localhost"
        // is refused too: it is a name, and names can be made to resolve elsewhere.
        outError = "Cloud requests require HTTPS; plain http is allowed only to 127.0.0.1 for testing.";
        return std::nullopt;
    }
    endpoint.basePath = std::move(path);
    return endpoint;
}

namespace
{
class RefusingTransport final : public CloudTransport
{
public:
    CloudHttpResult Post(const CloudHttpRequest&, const ChunkHandler&, std::stop_token) override
    {
        CloudHttpResult result;
        result.error = "This build has no HTTPS client for cloud requests (WinHTTP is Windows-only).";
        return result;
    }
};

#ifdef _WIN32
std::wstring Wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// Closes a handle exactly once, whichever of the reader and the stop callback gets there
// first. Closing the request handle from the callback is what makes a blocked read return.
void CloseOnce(std::atomic<HINTERNET>& handle)
{
    if (HINTERNET owned = handle.exchange(nullptr); owned != nullptr)
        WinHttpCloseHandle(owned);
}

class WinHttpTransport final : public CloudTransport
{
public:
    CloudHttpResult Post(const CloudHttpRequest& request, const ChunkHandler& onChunk, const std::stop_token stopToken) override
    {
        CloudHttpResult result;
        const std::wstring host = Wide(request.endpoint.host);
        const std::wstring path = Wide(request.path);
        std::string headerText;
        for (const auto& [name, value] : request.headers)
            headerText += name + ": " + value + "\r\n";
        const std::wstring headers = Wide(headerText);
        if (host.empty() || path.empty() || (!headerText.empty() && headers.empty()))
        {
            result.error = "The cloud request could not be encoded for WinHTTP.";
            return result;
        }

        std::atomic<HINTERNET> session{WinHttpOpen(L"Revia/0.2 optional-cloud-model", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        std::atomic<HINTERNET> connection{nullptr};
        std::atomic<HINTERNET> handle{nullptr};
        const auto closeAll = [&]()
        {
            CloseOnce(handle);
            CloseOnce(connection);
            CloseOnce(session);
        };
        if (session.load() == nullptr)
        {
            result.error = "Windows could not initialize the HTTPS client.";
            return result;
        }
        const int timeoutMs = std::max(1, request.timeoutSeconds) * 1000;
        WinHttpSetTimeouts(session.load(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        connection.store(WinHttpConnect(session.load(), host.c_str(), static_cast<INTERNET_PORT>(request.endpoint.port), 0));
        if (connection.load() != nullptr)
        {
            handle.store(WinHttpOpenRequest(connection.load(), L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, request.endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
        }
        if (handle.load() == nullptr)
        {
            result.error = "WinHTTP could not open the cloud request.";
            closeAll();
            return result;
        }
        // The audit names one host. A redirect would send the conversation somewhere else.
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(handle.load(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        const std::stop_callback cancel(stopToken, [&handle]() { CloseOnce(handle); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, request.timeoutSeconds));
        const BOOL sent = WinHttpSendRequest(handle.load(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            headers.empty() ? 0 : static_cast<DWORD>(-1L), const_cast<char*>(request.body.data()), static_cast<DWORD>(request.body.size()),
            static_cast<DWORD>(request.body.size()), 0);
        if (!sent || !WinHttpReceiveResponse(handle.load(), nullptr))
        {
            result.error = stopToken.stop_requested() ? "The cloud request was cancelled." : "The HTTPS request to the cloud model failed.";
            closeAll();
            return result;
        }
        DWORD status = 0;
        DWORD statusBytes = sizeof(status);
        WinHttpQueryHeaders(handle.load(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
            &statusBytes, WINHTTP_NO_HEADER_INDEX);
        result.bReceived = true;
        result.status = static_cast<int>(status);

        std::string chunk;
        while (!stopToken.stop_requested())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                result.error = "The cloud model did not finish within the configured timeout.";
                break;
            }
            DWORD available = 0;
            HINTERNET current = handle.load();
            if (current == nullptr || !WinHttpQueryDataAvailable(current, &available))
            {
                result.error = stopToken.stop_requested() ? "The cloud request was cancelled." : "The cloud response could not be read.";
                break;
            }
            if (available == 0)
                break;
            if (result.bytesReceived + available > MaximumResponseBytes)
            {
                result.error = "The cloud response exceeded the size limit.";
                break;
            }
            chunk.assign(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(current, chunk.data(), available, &read))
            {
                result.error = stopToken.stop_requested() ? "The cloud request was cancelled." : "The cloud response body could not be read.";
                break;
            }
            chunk.resize(read);
            result.bytesReceived += read;
            if (read > 0 && !onChunk(result.status, chunk))
                break;
        }
        if (stopToken.stop_requested() && result.error.empty())
            result.error = "The cloud request was cancelled.";
        closeAll();
        return result;
    }
};
#endif
} // namespace

std::shared_ptr<CloudTransport> MakePlatformCloudTransport()
{
#ifdef _WIN32
    return std::make_shared<WinHttpTransport>();
#else
    return std::make_shared<RefusingTransport>();
#endif
}

} // namespace revia::llm::cloud
