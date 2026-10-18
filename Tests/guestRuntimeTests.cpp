#include "Actions/actionRuntime.h"
#include "Core/runtimePath.h"
#include "Workspace/virtualMachineRuntime.h"
#include "Workspace/vmwareProvider.h"
#include "testSupport.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using namespace revia;
using tests::Check;
using namespace std::chrono_literals;

void Configure(const std::filesystem::path& path, bool read = true)
{
    const nlohmann::json data = {{"mode", "supervised"}, {"approvedRoots", nlohmann::json::array()}, {"createMissingApprovedRoots", false},
        {"guest", {{"enabled", true}, {"read", read}, {"write", true}, {"execute", true},
            {"allowTaskExecution", true}, {"approvedExecutables", {"/usr/bin/python3"}}}}};
    std::ofstream stream(path);
    stream << data.dump();
    stream.close();
    Check(!stream.fail(), "Could not write guest runtime fixture policy.");
}

void Initialize(actions::ActionRuntime& actions, const std::filesystem::path& root)
{
    std::string error;
    Check(actions.Initialize(root / "capabilities.json", root / "audit.jsonl", error), error);
}

void WaitReady(workspace::VirtualMachineRuntime& runtime, const std::chrono::seconds timeout)
{
    runtime.Start();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (runtime.Snapshot().state == workspace::VirtualMachineState::Preparing && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    const auto state = runtime.Snapshot();
    Check(state.state == workspace::VirtualMachineState::Ready, "Guest readiness failed: " + state.code);
}

actions::ActionRequest Request(const actions::ActionType type, std::string path)
{
    actions::ActionRequest request;
    request.id = actions::NewActionId();
    request.type = type;
    request.guest.operation = *actions::GuestOperationForAction(type);
    request.guest.path = std::move(path);
    return request;
}

class FixtureProvider final : public workspace::IVirtualMachineProvider
{
  public:
    int executions = 0;
    workspace::GuestRequest received;
    workspace::VirtualMachineSnapshot EnsureReady(std::stop_token) override
    { return {workspace::VirtualMachineState::Ready, "ready", "Ready", {"guest-environment", "guest-boot"}, false}; }
    workspace::GuestResult Execute(const workspace::GuestRequest& request, const workspace::GuestLimits&, std::stop_token) override
    {
        ++executions;
        received = request;
        workspace::GuestResult result;
        result.attempted = result.succeeded = true;
        result.receiptVerified = true;
        result.identity = request.expectedIdentity;
        result.content = "guest-only-content";
        result.code = "completed";
        return result;
    }
    workspace::GuestResult StopGuest() override
    { workspace::GuestResult result; result.succeeded = true; return result; }
};
}

void RunGuestRuntimeTests()
{
    tests::ScopedTestDirectory directory;
    Configure(directory.root / "capabilities.json");
    actions::ActionRuntime actions;
    Initialize(actions, directory.root);
    auto request = Request(actions::ActionType::VmReadTextFile, "same-name.txt");
    std::ofstream(directory.root / "same-name.txt") << "host-private-content";
    const auto unbound = actions.Execute(request, true);
    Check(!unbound.Succeeded() && !unbound.result.attempted && unbound.result.content.empty(),
        "An unbound VM action substituted the host filesystem.");
    auto provider = std::make_shared<FixtureProvider>();
    auto runtime = std::make_shared<workspace::VirtualMachineRuntime>(provider);
    WaitReady(*runtime, 2s);
    actions.BindGuestRuntime(runtime);
    const auto read = actions.Execute(request, true);
    Check(read.Succeeded() && read.result.guest && read.result.content == "guest-only-content" && provider->executions == 1 &&
        provider->received.expectedIdentity.bootId == "guest-boot", "Direct host admission did not bind and execute the guest request.");
    auto captured = actions.Settings();
    captured.guest.expectedIdentity = {"guest-environment", "old-boot"};
    request.guest.expectedIdentity = captured.guest.expectedIdentity;
    Check(!actions.ExecuteScoped(request, policy::CapabilityPolicy(captured), true).Succeeded() && provider->executions == 1,
        "A saved scope refreshed stale guest boot authority.");
    captured.guest.expectedIdentity = {};
    request.guest.expectedIdentity = {};
    Check(!actions.ExecuteScoped(request, policy::CapabilityPolicy(captured), true).Succeeded() && provider->executions == 1,
        "A legacy goal without guest identity gained VM authority.");
    actions.SetDispatchObserver([](const actions::ActionRequest&, bool beginning) { if (beginning) throw 1; });
    const auto beginFailure = actions.Execute(request, true);
    Check(!beginFailure.Succeeded() && !beginFailure.runtimeError.empty() && provider->executions == 1,
        "A failed dispatch notification reached guest execution.");
    actions.SetDispatchObserver([](const actions::ActionRequest&, bool beginning) { if (!beginning) throw 1; });
    const auto endFailure = actions.Execute(request, true);
    Check(!endFailure.Succeeded() && endFailure.result.succeeded && !endFailure.runtimeError.empty() && provider->executions == 2,
        "Guest execution truth was lost after a notification failure.");
    actions.SetDispatchObserver({});
    captured = actions.Settings();
    captured.guest.expectedIdentity = runtime->Snapshot().identity;
    request.guest.expectedIdentity = captured.guest.expectedIdentity;
    Configure(directory.root / "capabilities.json", false);
    Initialize(actions, directory.root);
    Check(!actions.ExecuteScoped(request, policy::CapabilityPolicy(captured), true).Succeeded() && provider->executions == 2,
        "A retained task bypassed revoked guest reading authority.");
    std::ifstream audit(directory.root / "audit.jsonl");
    const std::string records{std::istreambuf_iterator<char>(audit), std::istreambuf_iterator<char>()};
    Check(records.find("vm_read_text_file") != std::string::npos && records.find("guest-boot") != std::string::npos &&
        records.find("observer") != std::string::npos, "Guest receipt or notification failure was absent from durable audit evidence.");
    runtime->Stop();
    std::cout << "Guest runtime admission, revocation, identity, observer and audit tests passed.\n";
}

// Explicit manual qualification only; never invoked by the default deterministic suite.
void RunGuestLiveTests(const std::filesystem::path& runtimeRoot, const std::filesystem::path& python)
{
    const auto root = std::filesystem::canonical(runtimeRoot);
    tests::ScopedTestDirectory directory;
    Configure(directory.root / "capabilities.json");
    auto runtime = std::make_shared<workspace::VirtualMachineRuntime>(workspace::CreateVmwareProvider(
        {root / "RuntimeData/VirtualMachine/Guest", "legacy", std::filesystem::canonical(python),
            core::FindInstalledArtifact(root, "Tools/Guest/vmware_ubuntu.py")}));
    WaitReady(*runtime, 1805s);
    actions::ActionRuntime actions;
    Initialize(actions, directory.root);
    actions.BindGuestRuntime(runtime);
    const auto id = actions::NewActionId();
    const std::string program = "proof-" + id + ".py";
    const std::string artifact = "proof-" + id + ".txt";
    auto write = Request(actions::ActionType::VmWriteTextFile, program);
    write.guest.content = "from pathlib import Path\nassert sum(range(6)) == 15\nPath('" + artifact +
        "').write_text('verified\\n', encoding='utf-8')\nprint('guest test passed')\n";
    write.guest.expectedDigest = "missing";
    const auto written = actions.Execute(write, true);
    Check(written.Succeeded(), "Live guest source write failed: " + written.Message());
    const auto source = actions.Execute(Request(actions::ActionType::VmReadTextFile, program), true);
    Check(source.Succeeded() && source.result.content == write.guest.content, "Live source readback did not match the requested program.");
    auto execute = Request(actions::ActionType::VmExecuteProcess, "");
    execute.guest.executable = "/usr/bin/python3";
    execute.guest.workingDirectory = ".";
    execute.guest.arguments = {program};
    execute.guest.timeoutMs = 10000;
    const auto ran = actions.Execute(execute, true);
    Check(ran.Succeeded() && ran.result.guest && ran.result.guest->exitCode == 0 &&
        ran.result.guest->standardOutput.find("guest test passed") != std::string::npos, "Live code test did not return its real success receipt.");
    const auto verified = actions.Execute(Request(actions::ActionType::VmReadTextFile, artifact), true);
    Check(verified.Succeeded() && verified.result.content == "verified\n", "Live generated artifact was not independently verified.");
    std::cout << "LIVE VERIFIED ActionRuntime: source write/readback, Python assertion, exit/output and artifact readback; environment="
        << runtime->Snapshot().identity.environmentId << "; boot=" << runtime->Snapshot().identity.bootId << '\n';
    runtime->Stop();
    Check(runtime->Snapshot().state == workspace::VirtualMachineState::Stopped, "Live guest did not confirm graceful shutdown.");
    std::cout << "LIVE VERIFIED guest graceful shutdown.\n";
}
