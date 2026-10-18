#include "Workspace/virtualMachineExecutor.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace revia;
namespace
{
void Check(const bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
class Provider final : public workspace::IVirtualMachineProvider
{
  public:
    int calls = 0;
    workspace::GuestRequest received;
    workspace::GuestResult receipt;
    workspace::VirtualMachineSnapshot EnsureReady(std::stop_token) override
    {
        return {workspace::VirtualMachineState::Ready, "ready", "Ready", {"env", "boot"}};
    }
    workspace::GuestResult Execute(const workspace::GuestRequest& request, const workspace::GuestLimits&, std::stop_token) override
    {
        ++calls;
        received = request;
        return receipt;
    }
    workspace::GuestResult StopGuest() override { return {}; }
};
}

int main()
{
    try
    {
        auto provider = std::make_shared<Provider>();
        provider->receipt.attempted = provider->receipt.succeeded = true;
        provider->receipt.receiptVerified = true;
        provider->receipt.identity = {"env", "boot"};
        provider->receipt.content = "verified text";
        provider->receipt.digest = std::string(64, 'a');
        auto runtime = std::make_shared<workspace::VirtualMachineRuntime>(provider);
        runtime->Start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (runtime->Snapshot().state != workspace::VirtualMachineState::Ready && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        Check(runtime->Snapshot().state == workspace::VirtualMachineState::Ready, "Fixture guest did not start.");
        workspace::VirtualMachineExecutor executor(runtime);
        actions::ActionRequest request;
        request.type = actions::ActionType::VmReadTextFile;
        request.guest.operation = workspace::GuestOperation::ReadText;
        request.guest.path = "src/Main.cpp";
        request.guest.expectedIdentity = {"env", "boot"};
        request.beforeEffect = [](const std::string&) { return std::string{}; };
        actions::PolicyDecision decision;
        decision.verdict = actions::PolicyVerdict::Allowed;
        auto result = executor.Execute(request, decision);
        Check(result.succeeded && result.attempted && result.content == "verified text" && result.guest &&
            result.guest->digest == std::string(64, 'a') && provider->calls == 1 && provider->received.path == "src/Main.cpp",
            "Guest executor did not retain its provider receipt and exact guest request.");
        request.beforeEffect = [](const std::string&) { return "revoked"; };
        result = executor.Execute(request, decision);
        Check(!result.attempted && !result.succeeded && provider->calls == 1, "Guest executor ignored admission revocation.");
        request.beforeEffect = {};
        result = executor.Execute(request, decision);
        Check(!result.attempted && provider->calls == 1, "A guest request without a trusted submission guard reached the provider.");
        request.beforeEffect = [](const std::string&) { return std::string{}; };
        request.guest.expectedIdentity.bootId = "new-boot";
        result = executor.Execute(request, decision);
        Check(!result.attempted && !result.succeeded && provider->calls == 1, "Guest executor rebound a stale task to a current boot.");
        request.guest.expectedIdentity = {"env", "boot"};
        std::stop_source stop;
        stop.request_stop();
        request.executionStop = stop.get_token();
        result = executor.Execute(request, decision);
        Check(!result.attempted && !result.succeeded && provider->calls == 1, "Cancelled guest request reached the provider.");
        request.executionStop = {};
        request.type = actions::ActionType::VmExecuteProcess;
        request.guest.operation = workspace::GuestOperation::ExecuteProcess;
        provider->receipt.succeeded = false;
        provider->receipt.uncertainEffect = true;
        provider->receipt.exitCode = 1;
        provider->receipt.standardError = "test failed";
        result = executor.Execute(request, decision);
        Check(result.attempted && !result.succeeded && result.guest && result.guest->uncertainEffect &&
            result.guest->exitCode == 1 && result.guest->standardError == "test failed", "Guest failure lost its uncertainty or process receipt.");
        request.type = actions::ActionType::VmReadTextFile;
        result = executor.Execute(request, decision);
        Check(!result.attempted && provider->calls == 2, "Guest operation disguised as read reached process execution.");
        workspace::VirtualMachineExecutor unbound(nullptr);
        result = unbound.Execute(request, decision);
        Check(!result.attempted && !result.succeeded, "An unbound guest executor substituted a host execution path.");
        std::cout << "Virtual machine executor: receipt, admission, cancellation and no-fallback tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Virtual machine executor: " << error.what() << '\n';
        return 1;
    }
}
