#include "testSupport.h"
#include "Actions/actionRuntime.h"
#include "Stage/stageActionExecutor.h"
#include "Stage/stageChannel.h"
#include "Stage/stageProtocol.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using revia::actions::ActionRequest;
using revia::actions::ActionResult;
using revia::actions::ActionType;
using revia::stage::GuestInfo;
using revia::stage::StageChannelClient;
using revia::stage::StageGuestServer;
using revia::stage::StageTier;
using revia::tests::Check;
using namespace std::chrono_literals;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

ActionRequest Click(const std::string& application)
{
    ActionRequest request;
    request.id = "a1";
    request.type = ActionType::ClickPointer;
    request.application = application;
    request.windowTitle = "Notepad";
    request.input.x = 120;
    request.input.y = 44;
    request.input.hasPoint = true;
    request.input.button = ActionRequest::DesktopInput::PointerButton::Right;
    request.input.clickCount = 2;
    request.requestedBy = "goal";
    request.resolution.kind = revia::actions::TargetResolutionKind::UiaElement;
    request.resolution.resolvedName = "Save";
    request.resolution.resolvedRuntimeId = "42.7";
    request.resolution.regionLeft = 100;
    request.resolution.regionTop = 30;
    request.resolution.regionRight = 140;
    request.resolution.regionBottom = 58;
    return request;
}

void TestTheProtocolRoundTripsAndTiersAreKnown()
{
    ActionRequest inspect;
    inspect.type = ActionType::InspectWindow;
    inspect.application = "notepad.exe";
    Check(revia::stage::TierFor(inspect) == StageTier::Observe, "Inspecting is not T0.");
    Check(revia::stage::TierFor(Click("notepad.exe")) == StageTier::Confined, "A click in an app is not T1.");
    Check(revia::stage::TierFor(Click("")) == StageTier::Desktop, "A screen-space click is not T2.");
    ActionRequest launch;
    launch.type = ActionType::LaunchApplication;
    launch.application = "notepad.exe";
    Check(revia::stage::TierFor(launch) == StageTier::System, "Starting a process is not T3.");
    ActionRequest file;
    file.type = ActionType::ReadTextFile;
    Check(revia::stage::TierFor(file) == StageTier::System, "A non-desktop action was given a low tier.");
    StageTier parsed;
    Check(revia::stage::TierFromInt(2, parsed) && parsed == StageTier::Desktop && !revia::stage::TierFromInt(4, parsed),
        "Tier numbers are not read as they should be.");

    const std::string line = revia::stage::EncodeRequest("r1", Click("notepad.exe"), StageTier::Confined);
    ActionRequest back;
    StageTier tier;
    std::string error;
    Check(revia::stage::ReadRequest(line, back, tier, error), "A request did not read back: " + error);
    Check(tier == StageTier::Confined && back.id == "a1" && back.type == ActionType::ClickPointer &&
            back.application == "notepad.exe" && back.windowTitle == "Notepad" && back.input.x == 120 &&
            back.input.y == 44 && back.input.hasPoint && back.input.clickCount == 2 &&
            back.input.button == ActionRequest::DesktopInput::PointerButton::Right && back.requestedBy == "goal" &&
            back.resolution.kind == revia::actions::TargetResolutionKind::UiaElement &&
            back.resolution.resolvedRuntimeId == "42.7" && back.resolution.regionRight == 140,
        "The request lost fields on the way.");
    revia::stage::Envelope envelope;
    Check(revia::stage::Decode(line, envelope, error) && envelope.type == "request" && envelope.id == "r1",
        "The envelope was not read.");
    Check(!revia::stage::ReadRequest(R"({"type":"request","id":"x","tier":1,"action":{"type":"launch_rocket"}})",
              back, tier, error) && Contains(error, "unknown action type"),
        "An unknown action type was accepted.");
    Check(!revia::stage::ReadRequest("nonsense", back, tier, error), "Non-JSON was accepted as a request.");

    ActionResult result;
    result.attempted = true;
    result.succeeded = true;
    result.message = "clicked";
    result.content = std::string(100000, 'x');
    for (int index = 0; index < 300; ++index) result.entries.push_back("entry " + std::to_string(index));
    ActionResult readBack;
    Check(revia::stage::ReadResult(revia::stage::EncodeResult("r1", result), readBack, error) && readBack.attempted &&
            readBack.succeeded && readBack.message == "clicked" &&
            readBack.content.size() == revia::stage::LongestResultContent &&
            readBack.entries.size() == revia::stage::MostResultEntries,
        "A result did not read back bounded: " + error);
    GuestInfo guest;
    Check(revia::stage::ReadHello(revia::stage::EncodeHello({"g", "1", StageTier::Desktop, "clean"}), guest, error) &&
            guest.name == "g" && guest.grantedTier == StageTier::Desktop && guest.checkpoint == "clean",
        "The hello did not read back.");
    Check(!revia::stage::ReadHello(R"({"type":"hello","name":"g"})", guest, error) && Contains(error, "tier"),
        "A hello with no tier was accepted.");
    std::string reason;
    Check(revia::stage::ReadRefusal(revia::stage::EncodeRefusal("r1", "no"), reason) && reason == "no",
        "A refusal did not read back.");
}

void TestTheGuestPerformsWhatItsTierAllowsAndStopsOnHalt()
{
    StageGuestServer guest;
    std::mutex mutex;
    std::vector<std::string> performed;
    std::string error;
    Check(guest.Start("127.0.0.1", 0, {"fake-guest", "1", StageTier::Confined, "clean"},
              [&](const ActionRequest& request, const StageTier tier)
              {
                  {
                      std::lock_guard lock(mutex);
                      performed.push_back(revia::actions::ToString(request.type) + "@" + revia::stage::ToString(tier));
                  }
                  ActionResult result;
                  result.attempted = true;
                  result.succeeded = true;
                  result.message = "done in the guest";
                  result.content = request.application;
                  if (request.value == "slow") std::this_thread::sleep_for(1500ms);
                  return result;
              }, error),
        "The guest did not start: " + error);

    StageChannelClient client;
    Check(!client.IsConnected() && Contains(client.Execute(Click("notepad.exe"), StageTier::Confined, 1s).message, "not connected"),
        "An unconnected client did not say so.");
    Check(client.Connect("127.0.0.1", guest.Port(), 2s, error) && client.IsConnected(), "The client did not connect: " + error);
    Check(client.Guest().name == "fake-guest" && client.Guest().grantedTier == StageTier::Confined &&
            client.Guest().checkpoint == "clean",
        "The guest's hello was not kept.");
    Check(client.Ping(2s), "The guest did not answer a ping.");

    ActionResult inside = client.Execute(Click("notepad.exe"), StageTier::Confined, 5s);
    Check(inside.attempted && inside.succeeded && inside.message == "done in the guest" &&
            inside.content == "notepad.exe" && inside.backend == "stage:fake-guest",
        "A confined click was not performed in the guest: " + inside.message);
    ActionResult desktop = client.Execute(Click(""), StageTier::Desktop, 5s);
    Check(!desktop.attempted && Contains(desktop.message, "refused") && Contains(desktop.message, "T2 desktop") &&
            Contains(desktop.message, "started with T1"),
        "A desktop-wide click was performed by a T1 guest: " + desktop.message);
    // The host understating the tier does not help: the guest judges for itself.
    ActionResult understated = client.Execute(Click(""), StageTier::Observe, 5s);
    Check(!understated.attempted && Contains(understated.message, "T2 desktop"), "The guest took the host's word for the tier.");
    ActionRequest slow = Click("notepad.exe");
    slow.value = "slow";
    ActionResult late = client.Execute(slow, StageTier::Confined, 300ms);
    Check(!late.attempted && Contains(late.message, "did not answer in time"), "A slow guest was waited on for ever.");
    std::this_thread::sleep_for(1600ms);
    // The late answer must not be mistaken for the next request's.
    ActionResult next = client.Execute(Click("notepad.exe"), StageTier::Confined, 5s);
    Check(next.attempted && next.succeeded, "The next request after a timeout got the stale answer or none.");
    {
        std::lock_guard lock(mutex);
        Check(performed.size() == 3 && performed.front() == "click_pointer@T1 confined",
            "The guest performed the wrong things.");
    }
    Check(client.Halt(), "Halt could not be sent.");
    for (int slice = 0; slice < 50 && !guest.Halted(); ++slice) std::this_thread::sleep_for(20ms);
    Check(guest.Halted(), "The guest did not halt.");
    ActionResult afterHalt = client.Execute(Click("notepad.exe"), StageTier::Confined, 5s);
    Check(!afterHalt.attempted && Contains(afterHalt.message, "halted"), "A halted guest still performed.");
    Check(guest.Served() == 3, "The served count is wrong.");
    guest.Stop();
    ActionResult gone = client.Execute(Click("notepad.exe"), StageTier::Confined, 2s);
    Check(!gone.attempted && Contains(gone.message, "closed") && !client.IsConnected(),
        "A stopped guest was not noticed: " + gone.message);
}

void TestTheHostExecutorHoldsTheGrantedTierAndReconnects()
{
    StageGuestServer guest;
    std::string error;
    Check(guest.Start("127.0.0.1", 0, {"fake-guest", "1", StageTier::System, ""},
              [](const ActionRequest& request, StageTier)
              {
                  ActionResult result;
                  result.attempted = true;
                  result.succeeded = true;
                  result.message = "performed " + revia::actions::ToString(request.type);
                  return result;
              }, error),
        "The guest did not start: " + error);
    revia::actions::CapabilitySettings::Stage settings;
    settings.host = "127.0.0.1";
    settings.port = guest.Port();
    settings.grantedTier = 1;
    settings.timeoutSeconds = 5;
    auto client = std::make_shared<StageChannelClient>();
    revia::stage::StageActionExecutor executor(settings, client);
    Check(executor.Handles(ActionType::ClickPointer) && executor.Handles(ActionType::InspectWindow) &&
            executor.Handles(ActionType::LaunchApplication) && !executor.Handles(ActionType::ReadTextFile) &&
            !executor.Handles(ActionType::WebSearch),
        "The executor handles the wrong actions.");
    const ActionResult refused = executor.Execute(Click(""), {});
    Check(!refused.attempted && Contains(refused.message, "T2 desktop") && Contains(refused.message, "granted T1") &&
            !client->IsConnected(),
        "A request above the granted tier left the host: " + refused.message);
    const ActionResult performed = executor.Execute(Click("notepad.exe"), {});
    Check(performed.attempted && performed.succeeded && performed.message == "performed click_pointer" && client->IsConnected(),
        "A confined request was not performed through the executor: " + performed.message);
    // The guest goes away (a restore) and comes back on the same port: one reconnect.
    const std::uint16_t port = guest.Port();
    guest.Stop();
    StageGuestServer restored;
    Check(restored.Start("127.0.0.1", port, {"fake-guest", "1", StageTier::System, "clean"},
              [](const ActionRequest&, StageTier)
              {
                  ActionResult result;
                  result.attempted = true;
                  result.succeeded = true;
                  result.message = "after restore";
                  return result;
              }, error),
        "The guest could not be restarted on its port: " + error);
    const ActionResult again = executor.Execute(Click("notepad.exe"), {});
    Check(again.attempted && again.message == "after restore" && client->Guest().checkpoint == "clean",
        "The executor did not reconnect to the restored guest: " + again.message);
    restored.Stop();
    const ActionResult unreachable = executor.Execute(Click("notepad.exe"), {});
    Check(!unreachable.attempted && (Contains(unreachable.message, "not reachable") || Contains(unreachable.message, "closed")),
        "An unreachable stage was not reported: " + unreachable.message);
}

void TestTheRuntimeSendsDesktopActionsToTheStage()
{
    revia::tests::ScopedTestDirectory directory;
    StageGuestServer guest;
    std::string error;
    Check(guest.Start("127.0.0.1", 0, {"fake-guest", "1", StageTier::Confined, ""},
              [](const ActionRequest& request, StageTier)
              {
                  ActionResult result;
                  result.attempted = true;
                  result.succeeded = true;
                  result.message = "inspected " + request.application;
                  result.content = "Window: Untitled - Notepad";
                  return result;
              }, error),
        "The guest did not start: " + error);
    {
        const nlohmann::json capabilities = {
            {"mode", "supervised"},
            {"approvedRoots", {revia::actions::PathToUtf8(directory.root)}},
            {"approvedApplications", {"notepad.exe"}},
            {"approvedControls", {{"notepad.exe", {"*"}}}},
            {"autoApproveRiskThrough", "read_only"},
            {"createMissingApprovedRoots", false},
            {"stage", {{"enabled", true}, {"host", "127.0.0.1"}, {"port", guest.Port()}, {"grantedTier", 1}}}};
        std::ofstream file(directory.root / "capabilities.json");
        file << capabilities.dump();
    }
    revia::actions::ActionRuntime runtime;
    Check(runtime.Initialize(directory.root / "capabilities.json", directory.root / "audit.jsonl", error),
        "The runtime did not initialize with a stage: " + error);
    Check(runtime.Stage() != nullptr && runtime.Settings().stage.enabled, "The runtime did not keep the stage client.");
    ActionRequest inspect;
    inspect.id = revia::actions::NewActionId();
    inspect.type = ActionType::InspectWindow;
    inspect.application = "notepad.exe";
    inspect.requestedBy = "user";
    const revia::actions::ActionOutcome outcome = runtime.Execute(inspect);
    Check(outcome.Succeeded() && outcome.result.message == "inspected notepad.exe" &&
            outcome.result.content == "Window: Untitled - Notepad" && outcome.result.backend == "stage:fake-guest",
        "The runtime did not send the desktop action to the stage: " + outcome.Message() + " / " + outcome.policy.reason);
    Check(guest.Served() == 1, "The guest did not serve exactly one request.");
    // The audit log records it as any other action, with the stage as the backend.
    std::ifstream audit(directory.root / "audit.jsonl");
    std::string auditText((std::istreambuf_iterator<char>(audit)), std::istreambuf_iterator<char>());
    Check(Contains(auditText, "stage:fake-guest") || Contains(auditText, "inspect_window"),
        "The stage action was not audited.");
    guest.Stop();
}

} // namespace

void RunStageTests()
{
    TestTheProtocolRoundTripsAndTiersAreKnown();
    TestTheGuestPerformsWhatItsTierAllowsAndStopsOnHalt();
    TestTheHostExecutorHoldsTheGrantedTierAndReconnects();
    TestTheRuntimeSendsDesktopActionsToTheStage();
    std::cout << "Her desktop actions can run in a stage guest over a tiered channel both ends "
        "enforce, with a halt the guest never resumes from.\n";
}
