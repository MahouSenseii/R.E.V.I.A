#include "Actions/actionRuntime.h"
#include "Audit/contentDigest.h"
#include "Core/runtimePath.h"
#include "Policy/guestCapabilityPolicy.h"
#include "Workspace/guestReceipt.h"
#include "Workspace/virtualMachineRuntime.h"
#include "Workspace/vmwareProvider.h"
#include "testSupport.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
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

// Records every request a guest would receive and answers like a healthy sandbox.
class RecordingProvider final : public workspace::IVirtualMachineProvider
{
  public:
    std::vector<workspace::GuestRequest> received;
    workspace::VirtualMachineSnapshot EnsureReady(std::stop_token) override
    { return {workspace::VirtualMachineState::Ready, "ready", "Ready", {"guest-environment", "guest-boot"}, false}; }
    workspace::GuestResult Execute(const workspace::GuestRequest& request, const workspace::GuestLimits&, std::stop_token) override
    {
        received.push_back(request);
        workspace::GuestResult result;
        result.attempted = result.succeeded = result.receiptVerified = true;
        result.identity = request.expectedIdentity;
        result.exitCode = 0;
        result.networkIsolated = request.operation == workspace::GuestOperation::RunPython;
        result.standardOutput = "src/main.py:1:# TODO\n";
        result.code = "completed";
        return result;
    }
    workspace::GuestResult StopGuest() override
    { workspace::GuestResult result; result.succeeded = true; return result; }
};

void ConfigureCodeTools(const std::filesystem::path& path, const std::filesystem::path& approved)
{
    // Owner full access is the widest mode: what asks here asks everywhere.
    const nlohmann::json data = {{"mode", "owner_full_access"}, {"approvedRoots", {actions::PathToUtf8(approved)}},
        {"createMissingApprovedRoots", false}, {"autoApproveRiskThrough", "read_only"},
        {"guest", {{"enabled", true}, {"read", true}, {"write", true}, {"execute", true}, {"allowTaskExecution", false},
            {"approvedExecutables", {"/usr/bin/python3"}}}}};
    std::ofstream stream(path);
    stream << data.dump();
    stream.close();
    Check(!stream.fail(), "Could not write the code tool fixture policy.");
}

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    Check(!stream.fail(), "Could not write a share fixture file.");
}

void GuestCodeActionTests()
{
    tests::ScopedTestDirectory directory;
    const auto approved = directory.root / "approved";
    const auto project = approved / "project";
    Write(project / "src/main.py", "# TODO\nprint('hello')\n");
    Write(project / "README.md", "demo\n");
    Write(project / ".git/config", "[core]\n");
    Write(project / "node_modules/x.js", "module.exports = 1;\n");
    Write(project / ".env", "TOKEN=1\n");
    Write(project / "keys.txt", "-----BEGIN RSA PRIVATE KEY-----\nMIIBOgIBAAJBAKj34GkxFhD90vcNLYLInFEX6Ppy1tPf9Cnzj4p4WGeKLs1Pt8Qu\n-----END RSA PRIVATE KEY-----\n");
    Write(project / "blob.bin", std::string("PK\0\x03binary", 10));
    Write(project / "big.txt", std::string(70000, 'b'));
    for (int index = 0; index < 8; ++index)
        Write(project / ("data/part" + std::to_string(index) + ".txt"), std::string(60000, static_cast<char>('a' + index)));
    ConfigureCodeTools(directory.root / "capabilities.json", approved);
    actions::ActionRuntime actions;
    Initialize(actions, directory.root);
    auto provider = std::make_shared<RecordingProvider>();
    auto runtime = std::make_shared<workspace::VirtualMachineRuntime>(provider);
    WaitReady(*runtime, 2s);
    actions.BindGuestRuntime(runtime);

    // The interpreter: Destructive with its own confirmation, even in owner full access.
    auto run = Request(actions::ActionType::VmRunPython, "");
    run.guest.runId = "run-0123abcd";
    run.guest.code = "print(6 * 7)\n";
    run.guest.timeoutMs = 10000;
    Check(actions::RiskForAction(actions::ActionType::VmRunPython) == actions::RiskLevel::Destructive &&
              actions::AlwaysNeedsItsOwnConfirmation(actions::ActionType::VmRunPython),
        "An interpreter run was classified below a guest process.");
    Check(actions.Evaluate(run).verdict == actions::PolicyVerdict::RequiresConfirmation, "Owner full access ran model code without asking.");
    Check(!actions.Execute(run, false).Succeeded() && provider->received.empty(), "An unconfirmed interpreter run reached the guest.");
    const auto ran = actions.Execute(run, true);
    Check(ran.Succeeded() && provider->received.size() == 1 && provider->received.back().operation == workspace::GuestOperation::RunPython &&
              provider->received.back().code == run.guest.code && ran.result.guest && ran.result.guest->networkIsolated,
        "A confirmed interpreter run did not reach the guest as typed: " + ran.Message());
    for (const auto& bad : {std::string("../escape1"), std::string("short"), std::string("UPPER-case1")})
    {
        auto invalid = run;
        invalid.guest.runId = bad;
        Check(actions.Evaluate(invalid).verdict == actions::PolicyVerdict::Blocked, "A malformed scratch name was admitted: " + bad);
    }
    auto empty = run;
    empty.guest.code.clear();
    Check(actions.Evaluate(empty).verdict == actions::PolicyVerdict::Blocked, "An empty program was admitted.");
    policy::GuestCapabilitySettings narrow;
    narrow.enabled = narrow.execute = true;
    narrow.limits.approvedExecutables = {"/usr/bin/git"};
    Check(!policy::EvaluateGuestCapability(run.guest, narrow).allowed, "The interpreter ran without python3 on the allowlist.");
    narrow.limits.approvedExecutables = {"/usr/bin/python3"};
    Check(policy::EvaluateGuestCapability(run.guest, narrow).allowed, "An admitted interpreter run was refused.");
    narrow.execute = false;
    Check(!policy::EvaluateGuestCapability(run.guest, narrow).allowed, "The interpreter ran without guest execute permission.");

    // Searching is read-only and needs no prompt; its pattern and bounds are checked.
    auto search = Request(actions::ActionType::VmSearchFiles, "projects/demo");
    search.guest.pattern = "TODO";
    search.guest.timeoutMs = 5000;
    Check(actions.Evaluate(search).verdict == actions::PolicyVerdict::Allowed, "A read-only guest search asked or was refused.");
    const auto searched = actions.Execute(search, false);
    Check(searched.Succeeded() && searched.result.content.find("src/main.py:1:") != std::string::npos, "The search output was not returned.");
    auto noPattern = search;
    noPattern.guest.pattern.clear();
    auto noMatches = search;
    noMatches.guest.maxMatches = 0;
    Check(actions.Evaluate(noPattern).verdict == actions::PolicyVerdict::Blocked &&
              actions.Evaluate(noMatches).verdict == actions::PolicyVerdict::Blocked, "An unbounded search was admitted.");

    // Sharing: only an approved folder, always asked, copied as text files in batches.
    auto share = Request(actions::ActionType::VmShareProject, "projects/.incoming-share-0001");
    share.source = project;
    share.guest.publish = "demo";
    Check(actions.Evaluate(share).verdict == actions::PolicyVerdict::RequiresConfirmation, "Sharing host files did not ask.");
    auto outside = share;
    outside.source = directory.root;
    auto unnamed = share;
    unnamed.guest.publish.clear();
    auto badStaging = share;
    badStaging.guest.path = "projects/demo";
    auto hostRead = Request(actions::ActionType::VmReadTextFile, "a.txt");
    hostRead.source = approved;
    Check(actions.Evaluate(outside).verdict == actions::PolicyVerdict::Blocked &&
              actions.Evaluate(unnamed).verdict == actions::PolicyVerdict::Blocked &&
              actions.Evaluate(badStaging).verdict == actions::PolicyVerdict::Blocked &&
              actions.Evaluate(hostRead).verdict == actions::PolicyVerdict::Blocked,
        "A share outside an approved root, without a name or staging folder, or a host path on another guest action was admitted.");
    provider->received.clear();
    Check(!actions.Execute(share, false).Succeeded() && provider->received.empty(), "An unconfirmed share reached the guest.");
    const auto shared = actions.Execute(share, true);
    Check(shared.Succeeded(), "The confirmed share failed: " + shared.Message());
    std::set<std::string> names;
    for (std::size_t index = 0; index < provider->received.size(); ++index)
    {
        const auto& batch = provider->received[index];
        Check(batch.operation == workspace::GuestOperation::ImportFiles && batch.path == share.guest.path && batch.review.empty() &&
                  (batch.publish == "demo") == (index + 1 == provider->received.size()),
            "A share batch was misaddressed or published early.");
        for (const auto& file : batch.files)
            names.insert(file.path);
    }
    Check(provider->received.size() >= 2, "Large shares were not split into bounded batches.");
    Check(names.count("src/main.py") && names.count("README.md") && names.count("data/part7.txt") && !names.count(".git/config") &&
              !names.count("node_modules/x.js") && !names.count(".env") && !names.count("keys.txt") && !names.count("blob.bin") &&
              !names.count("big.txt"),
        "The share copied version control, dependencies, credentials, binaries or oversized files.");
    std::string skipped;
    for (const auto& entry : shared.result.entries)
        skipped += entry + "\n";
    Check(skipped.find(".env: named like a credential") != std::string::npos &&
              skipped.find("keys.txt: contains what looks like a credential") != std::string::npos &&
              skipped.find("blob.bin: not UTF-8 text") != std::string::npos,
        "The share did not say what it left out: " + skipped);

    // What the approval prompt shows.
    const std::string described = actions::DescribeGuestPayload(run);
    Check(described.find("no network") != std::string::npos && described.find("print(6 * 7)") != std::string::npos,
        "The approval text did not show the program and its sandbox.");
    auto longRun = run;
    longRun.guest.code.clear();
    for (int line = 0; line < 100; ++line)
        longRun.guest.code += "x = " + std::to_string(line) + "\n";
    Check(actions::DescribeGuestPayload(longRun).find("(40 more lines)") != std::string::npos, "A long program was not cut short.");
    Check(actions::DescribeGuestPayload(share).find(actions::PathToUtf8(project)) != std::string::npos, "The share prompt hid the folder.");

    // Files a run sends back are decoded and checked against their digests.
    std::string decoded;
    Check(workspace::DecodeBase64("YQ==", decoded) && decoded == "a" && workspace::DecodeBase64("YWI=", decoded) && decoded == "ab" &&
              !workspace::DecodeBase64("YW=j", decoded) && !workspace::DecodeBase64("YWJ", decoded) && !workspace::DecodeBase64("Y*Jj", decoded),
        "Base64 decoding accepted malformed input or refused valid input.");
    const auto artifact = [](const std::string& sha, const std::size_t bytes, const std::string& encoded)
    { return nlohmann::json{{"name", "plot.png"}, {"bytes", bytes}, {"sha256", sha}, {"contentBase64", encoded}}; };
    const auto parsed = workspace::ParseGuestArtifacts({{"artifacts", {artifact(audit::ContentDigest("abc"), 3, "YWJj")}}});
    Check(parsed.size() == 1 && parsed[0].bytes == "abc" && parsed[0].name == "plot.png", "A valid artifact was not decoded.");
    const auto refused = [](const nlohmann::json& receipt)
    {
        try
        {
            (void)workspace::ParseGuestArtifacts(receipt);
            return false;
        }
        catch (const std::runtime_error&)
        {
            return true;
        }
    };
    const auto good = artifact(audit::ContentDigest("abc"), 3, "YWJj");
    Check(refused({{"artifacts", {artifact(audit::ContentDigest("abd"), 3, "YWJj")}}}) &&
              refused({{"artifacts", {artifact(audit::ContentDigest("abc"), 4, "YWJj")}}}) && refused({{"artifacts", {good, good, good, good, good}}}) &&
              refused({{"artifacts", "plot.png"}}),
        "An artifact with a wrong digest or size, or too many artifacts, was believed.");

    std::ifstream audit(directory.root / "audit.jsonl");
    const std::string records{std::istreambuf_iterator<char>(audit), std::istreambuf_iterator<char>()};
    Check(records.find("vm_run_python") != std::string::npos && records.find("vm_share_project") != std::string::npos &&
              records.find("code_digest") != std::string::npos && records.find("network_isolated") != std::string::npos,
        "The code tools' actions were missing from the durable audit.");
    runtime->Stop();
    std::cout << "Guest code tool action policy, share, approval text and artifact receipt tests passed.\n";
}
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
    GuestCodeActionTests();
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
