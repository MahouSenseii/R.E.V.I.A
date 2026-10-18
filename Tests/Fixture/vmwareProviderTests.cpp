#include "Workspace/vmwareProvider.h"
#include "Actions/actionTypes.h"
#include "Process/processExecutor.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace revia::workspace;
namespace
{
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("revia-provider-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WaitFor(const std::filesystem::path& path)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(path) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Check(std::filesystem::exists(path), "The bounded provider child did not reach its checkpoint.");
}

void Publish(const std::filesystem::path& path, const std::string& value)
{
    auto temporary = path;
    temporary += ".tmp";
    { std::ofstream file(temporary); file << value; }
    std::filesystem::rename(temporary, path);
}

int LeaseOwner(const VmwareProviderOptions& options)
{
    auto provider = CreateVmwareProvider(options);
    const auto ready = provider->EnsureReady({});
    const auto control = options.root.parent_path();
    Publish(control / "owner-ready", ready.state == VirtualMachineState::Ready ? "ready" : "failed");
    Check(ready.state == VirtualMachineState::Ready, "Owner process could not acquire its enrollment.");
    WaitFor(control / "stop-owner");
    const auto stopped = provider->StopGuest();
    Publish(control / "owner-stopped", stopped.succeeded ? "stopped" : stopped.code);
    WaitFor(control / "exit-owner");
    // Deliberately bypass provider destruction: process exit must release the OS lease.
    std::_Exit(0);
}

void TestSeparateProcessesCannotShareShutdownOwnership(const std::filesystem::path& executable,
    VmwareProviderOptions options)
{
    options.root /= "lease-guest";
    const auto control = options.root.parent_path();
    revia::actions::ActionResult childResult;
    std::jthread owner([&](std::stop_token stop)
    {
        revia::actions::ActionRequest child;
        child.type = revia::actions::ActionType::ExecuteProcess;
        child.process.executable = std::filesystem::canonical(executable);
        child.process.workingDirectory = control;
        child.process.arguments = {"--lease-owner", revia::actions::PathToUtf8(options.pythonExecutable),
            revia::actions::PathToUtf8(options.bootstrapScript), revia::actions::PathToUtf8(options.root)};
        child.process.timeoutMs = 20000;
        for (const char* name : {"USERPROFILE", "APPDATA", "LOCALAPPDATA", "TEMP", "TMP", "ProgramFiles", "ProgramData", "SystemDrive"})
            if (const char* value = std::getenv(name)) child.process.environment[name] = value;
        child.executionStop = stop;
        child.beforeEffect = [stop](const std::string&) { return stop.stop_requested() ? "cancelled" : std::string{}; };
        revia::actions::PolicyDecision admitted;
        admitted.verdict = revia::actions::PolicyVerdict::Allowed;
        admitted.canonicalExecutable = child.process.executable;
        admitted.canonicalSource = control;
        admitted.processOutputLimitBytes = 4096;
        revia::process::ProcessExecutor executor;
        childResult = executor.Execute(child, admitted);
    });
    WaitFor(control / "owner-ready");
    Check(Read(control / "owner-ready") == "ready", "The separate provider owner failed readiness.");
    const auto before = Read(options.root / "bridge-calls");
    auto contender = CreateVmwareProvider(options);
    const auto denied = contender->EnsureReady({});
    GuestRequest request;
    request.operation = GuestOperation::WriteText;
    request.path = "file.txt";
    request.expectedIdentity = {"a37bb9b4-59c8-4ec6-aa8d-d5a238b9e823", "e27bb9b4-59c8-4ec6-aa8d-d5a238b9e823"};
    const auto execute = contender->Execute(request, {}, {});
    const auto stop = contender->StopGuest();
    const auto after = Read(options.root / "bridge-calls");
    std::string failures;
    if (denied.state != VirtualMachineState::Blocked) failures += "second process admitted readiness; ";
    if (execute.attempted || execute.succeeded) failures += "non-owner dispatched guest action; ";
    if (stop.attempted || stop.succeeded) failures += "non-owner dispatched shutdown; ";
    if (before != after) failures += "contender reached lifecycle bridge; ";
    Publish(control / "stop-owner", "1");
    WaitFor(control / "owner-stopped");
    if (Read(control / "owner-stopped") != "stopped") failures += "owner could not stop guest: " + Read(control / "owner-stopped") + "; ";
    const auto afterStop = Read(options.root / "bridge-calls");
    if (contender->EnsureReady({}).state != VirtualMachineState::Blocked || Read(options.root / "bridge-calls") != afterStop)
        failures += "lease ended before provider lifetime; ";
    Publish(control / "exit-owner", "1");
    owner.join();
    Check(childResult.succeeded, "Owner subprocess did not exit cleanly after bounded checkpoints.");
    contender.reset();
    auto successor = CreateVmwareProvider(options);
    if (successor->EnsureReady({}).state != VirtualMachineState::Ready) failures += "process exit did not release enrollment; ";
    if (!successor->StopGuest().succeeded) failures += "successor could not stop guest; ";
    if (!failures.empty()) throw std::runtime_error("Cross-process provider lease: " + failures);
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 5 && std::string(argv[1]) == "--lease-owner")
            return LeaseOwner({std::filesystem::path(argv[4]), "fixture-owner", std::filesystem::path(argv[2]),
                std::filesystem::path(argv[3])});
        Check(argc == 2, "Pass the verified Python interpreter path.");
        Fixture fixture;
        const auto script = fixture.root / "bridge.py";
        std::ofstream(script) << R"PY(import json, pathlib, sys
assert pathlib.Path.home().is_absolute()
command = sys.argv[1]
root = pathlib.Path(sys.argv[sys.argv.index('--root') + 1])
if command == 'ensure':
    root.mkdir(parents=True, exist_ok=True)
    if root.name == 'lease-guest' and not (root/'ensure-called').exists():
        assert not list(root.iterdir()), 'first provisioning received unknown files'
with (root/'bridge-calls').open('a') as calls: calls.write(command+'\n')
identity = {'environmentId':'a37bb9b4-59c8-4ec6-aa8d-d5a238b9e823','bootId':'e27bb9b4-59c8-4ec6-aa8d-d5a238b9e823'}
result = {'schemaVersion':1,'ok':True,'guestReady':True,'state':'ready','code':'ready',**identity}
if command == 'ensure':
    (root/'ensure-called').write_text('1')
elif command == 'status':
    result['guestReady'] = not (root/'stopped').exists()
    if (root/'changed-identity').exists(): result['bootId'] = 'e28bb9b4-59c8-4ec6-aa8d-d5a238b9e823'
elif command == 'request':
    request = json.loads(pathlib.Path(sys.argv[sys.argv.index('--request-file') + 1]).read_text())
    result.update(requestId=request['requestId'],attempted=True,succeeded=True,uncertainEffect=False,receiptVerified=True,content='guest text',digest='a'*64)
    if request['path'] == 'wrong': result['bootId'] = 'wrong-boot'
    if request['path'] == 'cancelled': result['cancelled'] = True
    if request['path'] == 'timeout': result['timedOut'] = True
    if request['path'] == 'nonzero':
        print(json.dumps(result)); sys.exit(2)
    if request['path'] == 'malformed':
        print('{'); sys.exit(0)
    if request['path'] == 'truncated':
        print('x'*1048577); sys.exit(0)
    if request['path'] == 'wrongtype': result['attempted'] = 'yes'
    if request['path'] == 'negative':
        result.update(ok=False,succeeded=False,code='write_conflict')
        print(json.dumps(result)); sys.exit(2)
elif command == 'stop':
    result.update(state='stopped',code='stopped')
    if (root/'stop-nonzero').exists():
        print(json.dumps(result)); sys.exit(2)
print(json.dumps(result))
)PY";
        VmwareProviderOptions options{fixture.root / "transport-guest", "fixture-owner", std::filesystem::path(argv[1]), script};
        std::filesystem::create_directory(options.root);
        TestSeparateProcessesCannotShareShutdownOwnership(std::filesystem::path(argv[0]), options);
        auto provider = CreateVmwareProvider(options);
        const auto ready = provider->EnsureReady({});
        Check(ready.state == VirtualMachineState::Ready, "Fixed bridge did not establish readiness.");
        GuestRequest request;
        request.operation = GuestOperation::ReadText;
        request.path = "file.txt";
        request.expectedIdentity = ready.identity;
        auto result = provider->Execute(request, {}, {});
        Check(result.succeeded && result.content == "guest text" && result.digest == std::string(64, 'a'), "Valid receipt fields were lost.");
        request.path = "negative";
        result = provider->Execute(request, {}, {});
        Check(!result.succeeded && result.code == "write_conflict", "Negative JSON receipt was lost on nonzero bridge exit.");
        request.path = "wrong";
        request.operation = GuestOperation::WriteText;
        result = provider->Execute(request, {}, {});
        Check(!result.succeeded && result.uncertainEffect, "Mismatched mutation receipt was accepted.");
        for (const char* fault : {"cancelled", "timeout", "nonzero", "malformed", "truncated", "wrongtype"})
        {
            request.path = fault;
            result = provider->Execute(request, {}, {});
            Check(!result.succeeded && result.uncertainEffect, "Invalid or interrupted effect receipt was accepted as success.");
        }
        for (const auto& entry : std::filesystem::directory_iterator(options.root))
            Check(entry.path().filename().string().find("request-") != 0, "Private request file survived completion.");
        std::ofstream(options.root / "changed-identity") << "1";
        Check(provider->EnsureReady({}).state == VirtualMachineState::Blocked, "Guest identity changed across readiness probes.");
        std::filesystem::remove(options.root / "changed-identity");
        std::ofstream(options.root / "stopped") << "1";
        Check(provider->EnsureReady({}).state == VirtualMachineState::Blocked, "Readiness survived failed post-job status.");
        std::stop_source cancelled;
        cancelled.request_stop();
        Check(provider->EnsureReady(cancelled.get_token()).state == VirtualMachineState::Blocked, "Cancelled setup established readiness.");
        Check(provider->StopGuest().succeeded, "Confirmed guest stop was lost.");
        std::ofstream(options.root / "stop-nonzero") << "1";
        Check(!provider->StopGuest().succeeded, "Failed stop transport accepted its contradictory success receipt.");
        std::cout << "VMware provider: transport, identity, cancellation, cleanup and post-job status passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
