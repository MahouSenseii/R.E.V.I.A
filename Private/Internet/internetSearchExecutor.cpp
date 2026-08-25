#include "Internet/internetSearchExecutor.h"
#include "Internet/internetBackend.h"
#include "Internet/visibleBrowserClient.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace revia::actions::internet
{

namespace
{
bool IsAutonomousCuriosityRequest(const ActionRequest& request)
{
    return request.requestedBy.rfind("autonomous_curiosity/", 0) == 0;
}

class ActiveBrowserRequest final
{
public:
    ActiveBrowserRequest(
        std::shared_ptr<VisibleBrowserCancellation> inputCancellation,
        const int port,
        const std::string& token)
        : cancellation(std::move(inputCancellation))
    {
        if (cancellation) requestId = cancellation->BeginRequest(port, token);
    }

    ~ActiveBrowserRequest()
    {
        if (cancellation) cancellation->EndRequest(requestId);
    }

    [[nodiscard]] bool IsCancelled() const
    {
        return cancellation && cancellation->IsCancelled(requestId);
    }

private:
    std::shared_ptr<VisibleBrowserCancellation> cancellation;
    std::uint64_t requestId = 0;
};

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool HostAllowed(
    const CapabilitySettings::InternetAccess& settings,
    const std::string& wanted)
{
    const std::string lowered = Lower(wanted);
    return std::any_of(
        settings.approvedHosts.begin(), settings.approvedHosts.end(),
        [&](const std::string& host) { return Lower(host) == lowered; });
}

std::string UrlEncode(const std::string& value)
{
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char character : value)
    {
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~')
        {
            encoded << static_cast<char>(character);
        }
        else
        {
            encoded << '%' << std::setw(2) << std::setfill('0')
                << static_cast<int>(character);
        }
    }
    return encoded.str();
}

#ifdef _WIN32
template <typename T>
void Close(T*& handle)
{
    if (handle != nullptr)
    {
        WinHttpCloseHandle(handle);
        handle = nullptr;
    }
}

bool GetHttps(
    const std::wstring& host,
    const std::wstring& path,
    const int timeoutMs,
    const std::size_t maxBytes,
    std::string& outBody,
    std::string& outError)
{
    HINTERNET session = WinHttpOpen(
        L"Revia/0.2 bounded-internet-lookup",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (session == nullptr)
    {
        outError = "Windows could not initialize the HTTPS client.";
        return false;
    }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection != nullptr
        ? WinHttpOpenRequest(
            connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
        : nullptr;
    const wchar_t* headers = L"Accept: application/json\r\n";
    const bool sent = request != nullptr &&
        WinHttpSendRequest(
            request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (!sent)
    {
        outError = "The HTTPS search request failed.";
        Close(request);
        Close(connection);
        Close(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusBytes,
        WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        outError = "The search provider returned HTTP " + std::to_string(status) + ".";
        Close(request);
        Close(connection);
        Close(session);
        return false;
    }

    outBody.clear();
    while (outBody.size() < maxBytes)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            outError = "The HTTPS response could not be read.";
            break;
        }
        if (available == 0)
        {
            break;
        }
        const std::size_t remaining = maxBytes - outBody.size();
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(available, remaining));
        std::string chunk(requested, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), requested, &read))
        {
            outError = "The HTTPS response body could not be read.";
            break;
        }
        chunk.resize(read);
        outBody += chunk;
        if (read < available && outBody.size() >= maxBytes)
        {
            outError = "The search response exceeded the configured byte limit.";
            break;
        }
    }
    Close(request);
    Close(connection);
    Close(session);
    return outError.empty() && !outBody.empty();
}
#endif

void AddRelatedTopics(
    const nlohmann::json& topics,
    const int maxResults,
    ActionResult& result)
{
    if (!topics.is_array())
    {
        return;
    }
    for (const auto& topic : topics)
    {
        if (static_cast<int>(result.entries.size()) >= maxResults)
        {
            return;
        }
        if (topic.contains("Topics"))
        {
            AddRelatedTopics(topic["Topics"], maxResults, result);
            continue;
        }
        const std::string text = topic.value("Text", "");
        const std::string url = topic.value("FirstURL", "");
        if (text.empty() || url.empty())
        {
            continue;
        }
        result.entries.push_back(url);
        if (!result.content.empty()) result.content += "\n\n";
        result.content += text + "\nSource: " + url;
    }
}
}

InternetSearchExecutor::InternetSearchExecutor(
    CapabilitySettings::InternetAccess inputSettings,
    std::shared_ptr<VisibleBrowserCancellation> inputCancellation)
    : settings(std::move(inputSettings)),
      cancellation(inputCancellation
          ? std::move(inputCancellation)
          : std::make_shared<VisibleBrowserCancellation>())
{
}

bool InternetSearchExecutor::Handles(const ActionType type) const
{
    return type == ActionType::WebSearch;
}

void InternetSearchExecutor::CancelActive()
{
    if (cancellation) cancellation->CancelActive();
}

bool InternetSearchExecutor::Admit(std::string& outReason)
{
    std::lock_guard lock(rateMutex);
    const auto now = std::chrono::steady_clock::now();
    const auto cutoff = now - std::chrono::minutes(1);
    while (!recentRequests.empty() && recentRequests.front() <= cutoff)
    {
        recentRequests.pop_front();
    }
    if (static_cast<int>(recentRequests.size()) >= settings.maxRequestsPerMinute)
    {
        outReason = "Internet lookup reached its configured rolling request limit.";
        return false;
    }
    recentRequests.push_back(now);
    outReason.clear();
    return true;
}

ActionResult InternetSearchExecutor::Execute(
    const ActionRequest& request,
    const PolicyDecision&)
{
    ActionResult result;
    result.attempted = true;
    result.backend = std::string(settings.visibleBrowser
        ? VisibleBrowserBackend
        : DuckDuckGoApiBackend);
    const bool autonomousCuriosity = IsAutonomousCuriosityRequest(request);
    if (!settings.enabled)
    {
        result.message = "Internet access is disabled.";
        return result;
    }
    if (autonomousCuriosity && !settings.visibleBrowser)
    {
        result.backend = std::string(VisibleBrowserBackend);
        result.message =
            "Autonomous research requires the dedicated visible browser; hidden API "
            "browsing was not used.";
        return result;
    }
    if (request.value.empty())
    {
        result.message = "Internet search requires a query.";
        return result;
    }
    std::string rateReason;
    if (!Admit(rateReason))
    {
        result.message = rateReason;
        return result;
    }
    std::string browserStartupFailure;
    if (settings.visibleBrowser)
    {
        std::lock_guard browserLock(browserMutex);
        std::unique_ptr<ActiveBrowserRequest> activeRequest;
        if (!browserProcess.IsRunning())
        {
            if (!browserProcess.Start(settings, browserStartupFailure))
            {
                // A missing worker, Node runtime, browser, or occupied service port is a
                // startup failure. Only this case may use the older bounded API backend.
            }
            else
            {
                activeRequest = std::make_unique<ActiveBrowserRequest>(
                    cancellation, browserProcess.Port(), browserProcess.Token());
                VisibleBrowserClient client(browserProcess.Port(), browserProcess.Token());
                if (!client.WaitUntilReady(
                        settings.visibleBrowserStartupTimeoutMs,
                        browserStartupFailure,
                        [&activeRequest]() { return activeRequest->IsCancelled(); }))
                {
                    browserProcess.Stop();
                }
            }
        }
        if (browserStartupFailure.empty() && browserProcess.IsRunning())
        {
            if (!activeRequest)
            {
                activeRequest = std::make_unique<ActiveBrowserRequest>(
                    cancellation, browserProcess.Port(), browserProcess.Token());
            }
            VisibleBrowserClient client(browserProcess.Port(), browserProcess.Token());
            // Once the visible service has started, request/navigation/extraction errors
            // are returned as-is. Silently switching to a hidden API after the visible
            // window failed mid-task would make the activity impossible to understand.
            ActionResult browserResult = client.Search(
                request.value,
                std::min(settings.maxResults, settings.visibleBrowserMaxPages),
                settings.maxResponseBytes,
                settings.visibleBrowserRequestTimeoutMs,
                settings.visibleBrowserStepDelayMs);
            browserResult.backend = std::string(VisibleBrowserBackend);
            if (activeRequest->IsCancelled() && !browserResult.succeeded)
            {
                browserResult.message = "The visible browser lookup was cancelled.";
            }
            return browserResult;
        }
        if (browserStartupFailure.empty())
        {
            browserStartupFailure = "The visible browser worker exited during startup.";
        }
        if (autonomousCuriosity)
        {
            result.backend = std::string(VisibleBrowserBackend);
            result.message = "Visible browser startup failed (" + browserStartupFailure +
                "). Autonomous research was not sent through a hidden API fallback.";
            return result;
        }
    }
    const auto apiResult = [&browserStartupFailure](ActionResult fallback)
    {
        if (!browserStartupFailure.empty())
        {
            fallback.message = "Visible browser startup failed (" + browserStartupFailure +
                "). Bounded API fallback: " + fallback.message;
        }
        return fallback;
    };
    if (!HostAllowed(settings, "api.duckduckgo.com"))
    {
        result.backend = std::string(DuckDuckGoApiBackend);
        result.message = "The configured search provider is outside the approved host list.";
        return apiResult(std::move(result));
    }
#ifdef _WIN32
    result.backend = std::string(DuckDuckGoApiBackend);
    const std::string encoded = UrlEncode(request.value);
    const std::string target = "/?q=" + encoded +
        "&format=json&no_html=1&no_redirect=1&skip_disambig=0";
    std::wstring wideTarget(target.begin(), target.end());
    std::string body;
    std::string error;
    if (!GetHttps(
            L"api.duckduckgo.com",
            wideTarget,
            settings.requestTimeoutMs,
            settings.maxResponseBytes,
            body,
            error))
    {
        result.message = error;
        return apiResult(std::move(result));
    }
    result = ParseDuckDuckGoResponse(body, settings.maxResults);
    result.attempted = true;
    result.backend = std::string(DuckDuckGoApiBackend);
    if (result.succeeded || !HostAllowed(settings, "en.wikipedia.org"))
    {
        return apiResult(std::move(result));
    }

    if (!Admit(rateReason))
    {
        result.message += " Wikipedia fallback was not sent: " + rateReason;
        return apiResult(std::move(result));
    }

    // DuckDuckGo's Instant Answer API intentionally does not behave like a full
    // search index. A zero-result response may therefore fall back to a second,
    // independently allow-listed read-only knowledge API. The query remains the
    // only model-controlled input; neither the host nor request path is exposed.
    const int boundedResults = std::clamp(settings.maxResults, 1, 20);
    const std::string wikipediaTarget =
        "/w/api.php?action=query&generator=search&gsrsearch=" + encoded +
        "&gsrlimit=" + std::to_string(boundedResults) +
        "&prop=extracts%7Cinfo&exintro=1&explaintext=1&exsentences=3"
        "&inprop=url&format=json&formatversion=2";
    const std::wstring wideWikipediaTarget(
        wikipediaTarget.begin(), wikipediaTarget.end());
    body.clear();
    error.clear();
    result.backend = std::string(WikipediaApiBackend);
    if (!GetHttps(
            L"en.wikipedia.org",
            wideWikipediaTarget,
            settings.requestTimeoutMs,
            settings.maxResponseBytes,
            body,
            error))
    {
        result.message += " Wikipedia fallback failed: " + error;
        return apiResult(std::move(result));
    }
    result = ParseWikipediaResponse(body, settings.maxResults);
    result.attempted = true;
    result.backend = std::string(WikipediaApiBackend);
    return apiResult(std::move(result));
#else
    result.message = "Internet lookup is currently implemented with Windows HTTPS.";
    return apiResult(std::move(result));
#endif
}

ActionResult InternetSearchExecutor::ParseDuckDuckGoResponse(
    const std::string& body,
    const int maxResults)
{
    ActionResult result;
    result.attempted = true;
    try
    {
        const nlohmann::json data = nlohmann::json::parse(body);
        const std::string abstract = data.value("AbstractText", "");
        const std::string abstractUrl = data.value("AbstractURL", "");
        const std::string heading = data.value("Heading", "");
        if (!abstract.empty() && !abstractUrl.empty())
        {
            result.content = (heading.empty() ? std::string() : heading + ": ") + abstract +
                "\nSource: " + abstractUrl;
            result.entries.push_back(abstractUrl);
        }
        AddRelatedTopics(data.value("RelatedTopics", nlohmann::json::array()), maxResults, result);
        result.succeeded = !result.content.empty();
        result.message = result.succeeded
            ? "Internet lookup returned " + std::to_string(result.entries.size()) +
                (result.entries.size() == 1 ? " source." : " sources.")
            : "The search provider returned no grounded results.";
        return result;
    }
    catch (const std::exception& error)
    {
        result.message = std::string("The search response was invalid JSON: ") + error.what();
        return result;
    }
}

ActionResult InternetSearchExecutor::ParseWikipediaResponse(
    const std::string& body,
    const int maxResults)
{
    ActionResult result;
    result.attempted = true;
    try
    {
        const nlohmann::json data = nlohmann::json::parse(body);
        if (!data.contains("query") || !data["query"].is_object() ||
            !data["query"].contains("pages") || !data["query"]["pages"].is_array())
        {
            result.message = "The approved knowledge provider returned no grounded results.";
            return result;
        }

        const int boundedResults = std::clamp(maxResults, 1, 20);
        for (const auto& page : data["query"]["pages"])
        {
            if (static_cast<int>(result.entries.size()) >= boundedResults)
            {
                break;
            }
            const std::string title = page.value("title", "");
            const std::string extract = page.value("extract", "");
            const std::string url = page.value("fullurl", "");
            if (title.empty() || extract.empty() || url.empty())
            {
                continue;
            }
            if (!result.content.empty())
            {
                result.content += "\n\n";
            }
            result.content += title + ": " + extract + "\nSource: " + url;
            result.entries.push_back(url);
        }
        result.succeeded = !result.entries.empty();
        result.message = result.succeeded
            ? "Internet lookup returned " + std::to_string(result.entries.size()) +
                (result.entries.size() == 1 ? " grounded source." : " grounded sources.")
            : "The approved knowledge provider returned no grounded results.";
        return result;
    }
    catch (const std::exception& error)
    {
        result.message = std::string("The knowledge response was invalid JSON: ") + error.what();
        return result;
    }
}

} // namespace revia::actions::internet
