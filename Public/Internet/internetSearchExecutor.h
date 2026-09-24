#pragma once

#include "Actions/IActionExecutor.h"
#include "Internet/visibleBrowserClient.h"
#include "Internet/visibleBrowserProcess.h"

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
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

private:
    [[nodiscard]] bool Admit(std::string& outReason);

    CapabilitySettings::InternetAccess settings;
    std::mutex rateMutex;
    std::mutex browserMutex;
    std::deque<std::chrono::steady_clock::time_point> recentRequests;
    VisibleBrowserProcess browserProcess;
    std::shared_ptr<VisibleBrowserCancellation> cancellation;
};

} // namespace revia::actions::internet
