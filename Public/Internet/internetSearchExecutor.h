#pragma once

#include "Actions/IActionExecutor.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace revia::actions::internet
{

class InternetSearchExecutor final : public IActionExecutor
{
public:
    explicit InternetSearchExecutor(CapabilitySettings::InternetAccess settings);

    [[nodiscard]] bool Handles(ActionType type) const override;
    [[nodiscard]] ActionResult Execute(
        const ActionRequest& request,
        const PolicyDecision& decision) override;

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
    std::deque<std::chrono::steady_clock::time_point> recentRequests;
};

} // namespace revia::actions::internet
