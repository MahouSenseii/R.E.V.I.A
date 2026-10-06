#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace revia::browser
{

// Owns private protocol pipes, the worker Job Object and one isolated browser profile.
class BrowserSession
{
  public:
    BrowserSession(std::filesystem::path worker, std::filesystem::path privateRoot);
    ~BrowserSession();
    BrowserSession(const BrowserSession&) = delete;
    BrowserSession& operator=(const BrowserSession&) = delete;

    [[nodiscard]] actions::ActionResult Execute(const actions::ActionRequest& request, const BrowserSettings& settings);
    void Stop();
    [[nodiscard]] std::optional<BrowserReceipt> Observation() const;

  private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace revia::browser
