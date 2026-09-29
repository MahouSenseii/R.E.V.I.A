#include "LLM/httpsTransport.h"

#include <algorithm>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace revia::llm
{

namespace
{
#ifdef _WIN32
std::wstring Widen(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        wide.data(), length);
    return wide;
}

class WinHttpTransport final : public HttpsTransport
{
public:
    HttpsResponse Post(const HttpsRequest& request, const std::stop_token stopToken) override
    {
        HttpsResponse response;
        const HINTERNET session = WinHttpOpen(L"Revia/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr)
        {
            response.error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ").";
            return response;
        }
        const DWORD timeout = static_cast<DWORD>(std::max(1, request.timeoutSeconds) * 1000);
        WinHttpSetTimeouts(session, 10000, 10000, timeout, timeout);
        const HINTERNET connection = WinHttpConnect(session, Widen(request.host).c_str(),
            static_cast<INTERNET_PORT>(request.port), 0);
        if (connection == nullptr)
        {
            response.error = "WinHttpConnect failed (" + std::to_string(GetLastError()) + ").";
            WinHttpCloseHandle(session);
            return response;
        }
        const HINTERNET handle = WinHttpOpenRequest(connection, L"POST",
            Widen(request.path).c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (handle == nullptr)
        {
            response.error = "WinHttpOpenRequest failed (" + std::to_string(GetLastError()) + ").";
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }
        // Cancellation closes the request handle from the stopping thread, which aborts
        // whatever WinHTTP is waiting on; the closed flag keeps this thread from closing
        // it twice.
        std::mutex closeMutex;
        bool closed = false;
        const auto closeRequest = [&]
        {
            std::lock_guard lock(closeMutex);
            if (closed) return;
            closed = true;
            WinHttpCloseHandle(handle);
        };
        std::stop_callback cancel(stopToken, [&] { closeRequest(); });
        std::wstring headerBlock;
        for (const auto& [name, value] : request.headers)
        {
            headerBlock += Widen(name) + L": " + Widen(value) + L"\r\n";
        }
        const BOOL sent = WinHttpSendRequest(handle,
            headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
            headerBlock.empty() ? 0 : static_cast<DWORD>(-1L),
            request.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                 : const_cast<char*>(request.body.data()),
            static_cast<DWORD>(request.body.size()),
            static_cast<DWORD>(request.body.size()), 0);
        if (!sent || !WinHttpReceiveResponse(handle, nullptr))
        {
            response.error = stopToken.stop_requested()
                ? "The request was cancelled."
                : "The request did not complete (WinHTTP error " +
                    std::to_string(GetLastError()) + ").";
            closeRequest();
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(status);
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(handle, &available)) break;
            if (available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(handle, chunk.data(), available, &read)) break;
            response.body.append(chunk.data(), read);
        }
        response.completed = !stopToken.stop_requested();
        if (!response.completed) response.error = "The request was cancelled.";
        closeRequest();
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }
    std::string Describe() const override { return "WinHTTP (system TLS)"; }
    bool Available() const override { return true; }
};
#else
class UnavailableTransport final : public HttpsTransport
{
public:
    HttpsResponse Post(const HttpsRequest&, std::stop_token) override
    {
        HttpsResponse response;
        response.error = "HTTPS requests leave this machine through Windows' WinHTTP; "
                         "there is no TLS transport on this platform.";
        return response;
    }
    std::string Describe() const override { return "no TLS transport on this platform"; }
    bool Available() const override { return false; }
};
#endif
} // namespace

std::unique_ptr<HttpsTransport> MakeSystemHttpsTransport()
{
#ifdef _WIN32
    return std::make_unique<WinHttpTransport>();
#else
    return std::make_unique<UnavailableTransport>();
#endif
}

} // namespace revia::llm
