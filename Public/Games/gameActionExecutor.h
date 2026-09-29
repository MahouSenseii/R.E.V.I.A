#pragma once

#include "Actions/IActionExecutor.h"
#include "Games/neuroGameServer.h"

#include <chrono>
#include <memory>

namespace revia::games
{

// A game action as a typed action: the policy admits it, this sends it to the game
// and waits for the game's verdict, and the audit log records both, like everything
// else she does.
class GameActionExecutor final : public actions::IActionExecutor
{
public:
    // The protocol discards a result that takes longer than about twenty seconds.
    static constexpr std::chrono::seconds ResultTimeout{20};

    explicit GameActionExecutor(std::shared_ptr<NeuroGameServer> server);
    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) override;

private:
    std::shared_ptr<NeuroGameServer> server;
};

} // namespace revia::games
