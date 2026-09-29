// The Revia-Stage guest: her hands, in a machine that is not the one she thinks in.
//
//   ReviaStageGuest.exe --capabilities <capabilities.json> [--host 0.0.0.0] [--port 39610]
//                       [--tier 1] [--checkpoint <name>]
//
// Runs inside the Hyper-V guest and serves the stage channel (Stage/stageProtocol.h):
// the host sends typed desktop actions its policy already admitted, this performs
// them with the same Windows executors the host would have used on itself, and
// reports back. It holds no model, no memory, no credentials and no policy of its
// own beyond the tier it was started with, which it enforces on every request
// whatever the host said. Windows only, like the executors it wraps.
#include "Actions/actionTypes.h"
#include "Policy/permissionStore.h"
#include "Stage/stageChannel.h"
#include "Windows/desktopControlExecutor.h"
#include "Windows/windowsAutomationExecutor.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::atomic<bool> keepRunning = true;

void OnSignal(int)
{
    keepRunning.store(false);
}
} // namespace

int main(const int argc, char** argv)
{
    std::string capabilities;
    std::string host = "0.0.0.0";
    int port = 39610;
    int tier = 1;
    std::string checkpoint;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const auto next = [&]() -> std::string { return index + 1 < argc ? argv[++index] : std::string(); };
        if (argument == "--capabilities") capabilities = next();
        else if (argument == "--host") host = next();
        else if (argument == "--port") port = std::atoi(next().c_str());
        else if (argument == "--tier") tier = std::atoi(next().c_str());
        else if (argument == "--checkpoint") checkpoint = next();
        else
        {
            std::cerr << "Unknown argument " << argument << "\n";
            return 2;
        }
    }
    if (capabilities.empty())
    {
        std::cerr << "Give --capabilities <capabilities.json>: the guest performs only what its own "
                     "capability file allows, exactly as the host would.\n";
        return 2;
    }
    revia::actions::CapabilitySettings settings;
    std::string error;
    if (!revia::policy::PermissionStore{}.Load(capabilities, settings, error))
    {
        std::cerr << "The capability file could not be read: " << error << "\n";
        return 2;
    }
    revia::stage::GuestInfo self;
    self.name = "revia-stage-guest";
    self.version = "1";
    self.checkpoint = checkpoint;
    if (!revia::stage::TierFromInt(tier, self.grantedTier))
    {
        std::cerr << "--tier must be 0, 1, 2 or 3.\n";
        return 2;
    }

    // The same executors the host registers for itself, behind the same guard and
    // approval gate: a request that would be refused on the host is refused here.
    auto guard = std::make_shared<revia::policy::DesktopInputGuard>();
    auto approvals = std::make_shared<revia::policy::DesktopApprovalGate>();
    std::vector<std::unique_ptr<revia::actions::IActionExecutor>> executors;
    executors.push_back(std::make_unique<revia::actions::windows::WindowsAutomationExecutor>(
        settings.desktopControl, approvals));
    executors.push_back(std::make_unique<revia::actions::windows::DesktopControlExecutor>(
        settings.desktopControl, guard, approvals));

    revia::stage::StageGuestServer server;
    const bool started = server.Start(host, static_cast<std::uint16_t>(port), self,
        [&executors](const revia::actions::ActionRequest& request, revia::stage::StageTier)
        {
            for (const auto& executor : executors)
            {
                if (executor->Handles(request.type))
                {
                    revia::actions::PolicyDecision decision;
                    decision.verdict = revia::actions::PolicyVerdict::Allowed;
                    decision.reason = "Admitted by the host's policy; performed in the stage.";
                    return executor->Execute(request, decision);
                }
            }
            revia::actions::ActionResult result;
            result.message = "No executor in the guest handles " + revia::actions::ToString(request.type) + ".";
            return result;
        }, error);
    if (!started)
    {
        std::cerr << "The stage guest could not listen: " << error << "\n";
        return 1;
    }
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::cout << "Revia-Stage guest listening on " << host << ":" << server.Port() << " at "
              << revia::stage::ToString(self.grantedTier) << ". Ctrl+C stops it.\n";
    while (keepRunning.load() && server.IsRunning())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (server.Halted())
        {
            std::cout << "Halted by the host; restart the guest to continue.\n";
            break;
        }
    }
    server.Stop();
    return 0;
}
