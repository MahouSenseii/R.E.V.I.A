#pragma once

#include "Actions/IActionExecutor.h"
#include "Stage/stageChannel.h"

#include <memory>

namespace revia::stage
{

// Desktop actions performed in the stage guest rather than on this machine. Registered
// ahead of the local desktop executors when the stage is on, so every request the
// policy admitted goes over the channel; the tier the owner granted is the last check
// before it leaves, and the guest's own is the first when it arrives.
class StageActionExecutor final : public actions::IActionExecutor
{
public:
    StageActionExecutor(actions::CapabilitySettings::Stage settings, std::shared_ptr<StageChannelClient> client);

    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) override;

private:
    bool EnsureConnected(std::string& outError);

    actions::CapabilitySettings::Stage settings;
    std::shared_ptr<StageChannelClient> client;
};

} // namespace revia::stage
