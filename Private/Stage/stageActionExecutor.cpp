#include "Stage/stageActionExecutor.h"

namespace revia::stage
{

StageActionExecutor::StageActionExecutor(
    actions::CapabilitySettings::Stage inputSettings, std::shared_ptr<StageChannelClient> inputClient)
    : settings(std::move(inputSettings)), client(std::move(inputClient))
{
}

bool StageActionExecutor::Handles(const actions::ActionType type) const
{
    return actions::IsDesktopControlAction(type) || actions::IsUiAutomationAction(type);
}

bool StageActionExecutor::EnsureConnected(std::string& outError)
{
    if (!client) 
    {
        outError = "No stage channel was created.";
        return false;
    }
    if (client->IsConnected()) return true;
    return client->Connect(settings.host, static_cast<std::uint16_t>(settings.port),
        std::chrono::seconds(std::max(1, settings.connectTimeoutSeconds)), outError);
}

actions::ActionResult StageActionExecutor::Execute(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision)
{
    (void)decision;
    actions::ActionResult result;
    result.backend = "stage";
    StageTier granted = StageTier::Observe;
    (void)TierFromInt(settings.grantedTier, granted);
    const StageTier needed = TierFor(request);
    if (static_cast<int>(needed) > static_cast<int>(granted))
    {
        result.message = std::string("This needs ") + ToString(needed) + " in the stage and the owner granted " +
            ToString(granted) + "; raise stage.grantedTier in the capability file to allow it.";
        return result;
    }
    std::string error;
    if (!EnsureConnected(error))
    {
        result.message = "The stage is not reachable: " + error;
        return result;
    }
    actions::ActionResult performed = client->Execute(
        request, needed, std::chrono::seconds(std::max(1, settings.timeoutSeconds)));
    if (!performed.attempted && !client->IsConnected())
    {
        // One reconnect, once: a guest restored from its checkpoint answers again.
        if (EnsureConnected(error))
        {
            performed = client->Execute(request, needed, std::chrono::seconds(std::max(1, settings.timeoutSeconds)));
        }
    }
    return performed;
}

} // namespace revia::stage
