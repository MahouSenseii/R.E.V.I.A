#pragma once

#include "Actions/IActionExecutor.h"
#include "Browser/browserSession.h"

#include <memory>
#include <utility>

namespace revia::browser
{

class BrowserExecutor final : public actions::IActionExecutor
{
  public:
    explicit BrowserExecutor(std::shared_ptr<BrowserSession> session) : session(std::move(session))
    {
    }
    [[nodiscard]] bool Handles(actions::ActionType type) const override
    {
        return actions::IsBrowserAction(type);
    }
    [[nodiscard]] actions::ActionResult Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision) override;

  private:
    std::shared_ptr<BrowserSession> session;
};

} // namespace revia::browser
