#pragma once

#include "Actions/IActionExecutor.h"
#include "Internet/visibleBrowserClient.h"
#include "Internet/visibleBrowserProcess.h"
#include "LLM/httpsTransport.h"

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace revia::actions::internet
{

class InternetSearchExecutor final : public IActionExecutor
{
public:
    explicit InternetSearchExecutor(
        CapabilitySettings::InternetAccess settings,
        std::shared_ptr<VisibleBrowserCancellation> cancellation = {});

    [[nodiscard]] bool Handles(ActionType type) const override;
    [[nodiscard]] ActionResult Execute(
        const ActionRequest& request,
        const PolicyDecision& decision) override;
    void CancelActive();

    // Kept public for deterministic parser tests; it performs no network access.
    [[nodiscard]] static ActionResult ParseDuckDuckGoResponse(
        const std::string& body,
        int maxResults);
    [[nodiscard]] static ActionResult ParseWikipediaResponse(
        const std::string& body,
        int maxResults);
    // The other providers' answers, each into the same shape the visible browser
    // writes -- a title line, "URL:", the snippet, "Source:" -- so the reader splits
    // them the same way.
    [[nodiscard]] static ActionResult ParseSearxngResponse(const std::string& body, int maxResults);
    [[nodiscard]] static ActionResult ParseBraveResponse(const std::string& body, int maxResults);
    [[nodiscard]] static ActionResult ParseTavilyResponse(const std::string& body, int maxResults);
    // The request a keyed provider gets, key included: for the tests, which use a fake.
    [[nodiscard]] static revia::llm::HttpsRequest BuildProviderRequest(
        const CapabilitySettings::InternetAccess& settings,
        const std::string& query,
        const std::string& key);
    // Which host a provider talks to; empty for a provider that does not exist.
    [[nodiscard]] static std::string ProviderHost(const CapabilitySettings::InternetAccess& settings);
    // The transport a keyed provider leaves through, and its key: seams for the tests.
    // Without them the OS's TLS is used and the key is read from the secret store or
    // the environment on first use.
    void SetHttpsTransport(std::unique_ptr<revia::llm::HttpsTransport> transport);
    void SetProviderKey(std::string key);

private:
    [[nodiscard]] bool Admit(std::string& outReason);
    // One bounded search through the configured provider when it is not DuckDuckGo.
    // A failure comes back as a failed result whose message says why, and the caller
    // goes on to the DuckDuckGo and Wikipedia fallback.
    [[nodiscard]] ActionResult SearchProvider(const std::string& query);
    [[nodiscard]] std::string ProviderKey(std::string& outError);

    CapabilitySettings::InternetAccess settings;
    std::unique_ptr<revia::llm::HttpsTransport> transport;
    std::mutex keyMutex;
    std::optional<std::string> providerKey;
    std::mutex rateMutex;
    std::mutex browserMutex;
    std::deque<std::chrono::steady_clock::time_point> recentRequests;
    VisibleBrowserProcess browserProcess;
    std::shared_ptr<VisibleBrowserCancellation> cancellation;
};

} // namespace revia::actions::internet
