#include "Actions/actionRuntime.h"
#include "Agents/agentToolWorker.h"
#include "Audit/contentDigest.h"
#include "testSupport.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

void RunAgentToolActionTests()
{
    using namespace revia;
    using tests::Check;
    tests::ScopedTestDirectory directory;
    std::filesystem::path executable;
#ifdef _WIN32
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    executable = std::filesystem::path(module).parent_path() / "ReviaProcessFixture.exe";
#endif
    Check(std::filesystem::exists(executable), "Worker native test requires the disposable process fixture.");
    const auto root = actions::PathToUtf8(directory.root);
    const auto file = directory.root / "worker.txt";
    const nlohmann::json config = {{"mode", "supervised"}, {"approvedRoots", {root}}, {"createMissingApprovedRoots", false},
        {"autoApproveRiskThrough", "reversible_write"},
        {"process", {{"enabled", true}, {"allowTaskExecution", true}, {"approvedExecutables", {actions::PathToUtf8(executable)}},
                        {"maxTimeoutMs", 3000}, {"maxOutputBytes", 512}}}};
    std::ofstream(directory.root / "capabilities.json") << config.dump();
    actions::ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(directory.root / "capabilities.json", directory.root / "audit.jsonl", error), error);
    auto authority = std::make_shared<policy::CompanionAuthority>();
    const runtime::RuntimeStamp session{"worker-companion", "worker-session", 1, {}, {}, 1};
    Check(authority->RegisterSession(session), "Worker session authority registration failed.");
    authority->SetCompanionDefaults(session.companionId, policy::AuthorityPermissions::WithinMachineCeiling());
    auto parent = session;
    parent.taskId = "workflow";
    auto child = parent;
    child.taskId = child.attemptId = "workflow.worker.1";
    policy::AuthorityPermissions permissions;
    permissions.operations = {actions::ActionType::ReadTextFile, actions::ActionType::ListDirectory, actions::ActionType::WriteTextFile,
        actions::ActionType::ExecuteProcess};
    permissions.roots = {directory.root};
    permissions.applications = {actions::PathToUtf8(executable)};
    Check(authority->RegisterTask(parent, {}, permissions) && authority->RegisterTask(child, parent.taskId, permissions),
        "Worker parent/attempt restriction registration failed.");
    runtime.BindAuthority(authority, session);
    const policy::CapabilityPolicy captured(runtime.Settings());
    const auto execute = [&](const nlohmann::json& tool, bool admitted = true)
    {
        std::optional<actions::ActionRequest> request;
        Check(agents::ParseWorkerToolResponse(nlohmann::json{{"tool", tool}}.dump(), true, true, request, error) && request.has_value(),
            error);
        request->beforeEffect = [admitted](const std::string&) { return admitted ? std::string{} : std::string("Attempt retired."); };
        const auto current = runtime.Settings().process;
        const bool delegatedProcess = request->type == actions::ActionType::ExecuteProcess && captured.Settings().process.enabled &&
                                      captured.Settings().process.allowTaskExecution && current.enabled && current.allowTaskExecution;
        return runtime.ExecuteScopedFor(child, *request, captured, delegatedProcess);
    };
    nlohmann::json write = {
        {"action", "write_text_file"}, {"source", actions::PathToUtf8(file)}, {"content", "first"}, {"expected_digest", "missing"}};
    Check(execute(write).Succeeded(), "Admitted typed worker write did not reach the native executor.");
    write["content"] = "second";
    Check(!execute(write).Succeeded(), "Worker missing-file expectation overwrote an existing file.");
    write["expected_digest"] = audit::ContentDigest("first");
    Check(!execute(write, false).Succeeded(), "Retired worker admission changed the native file.");
    Check(execute(write).Succeeded(), "Matching worker content digest did not admit replacement.");
    const auto read = execute({{"action", "read_text_file"}, {"source", actions::PathToUtf8(file)}});
    Check(read.Succeeded() && read.result.content == "second", "Typed worker read lost native result.");
    Check(execute({{"action", "list_directory"}, {"source", root}}).Succeeded(), "Typed worker listing did not execute.");
    const auto receiptFor = [&](const nlohmann::json& tool)
    {
        std::optional<actions::ActionRequest> request;
        Check(agents::ParseWorkerToolResponse(nlohmann::json{{"tool", tool}}.dump(), false, false, request, error) && request,
            "The path-grounding fixture did not create a typed read-only action.");
        const auto outcome = runtime.ExecuteScopedFor(child, *request, captured, false);
        return nlohmann::json::parse(agents::WorkerToolReceipt(*request, outcome));
    };
    const auto firstDirectory = directory.root / "first";
    const auto secondDirectory = directory.root / "second";
    std::filesystem::create_directories(firstDirectory);
    std::filesystem::create_directories(secondDirectory);
    std::ofstream(firstDirectory / "shared.conf") << "FIRST_DIRECTORY_FACT";
    std::ofstream(secondDirectory / "shared.conf") << "SECOND_DIRECTORY_FACT";
    for (const auto& [location, fact] : std::vector<std::pair<std::filesystem::path, std::string>>{
             {firstDirectory, "FIRST_DIRECTORY_FACT"}, {secondDirectory, "SECOND_DIRECTORY_FACT"}})
    {
        const auto listing = receiptFor({{"action", "list_directory"}, {"source", actions::PathToUtf8(location)}});
        Check(listing.at("succeeded") == true && listing.at("output").get<std::string>().find("shared.conf") != std::string::npos,
            "The native listing did not expose the shared leaf name.");
        Check(listing.contains("source") && listing.at("source").is_string() &&
                  listing.at("source") == actions::PathToUtf8(location) && listing.contains("pathDigests") &&
                  listing.at("pathDigests").at("source") == audit::ContentDigest(actions::PathToUtf8(location)),
            "A native worker listing lost its structured directory identity.");
        const auto selected = actions::Utf8ToPath(listing.at("source").get<std::string>()) / "shared.conf";
        const auto loaded = receiptFor({{"action", "read_text_file"}, {"source", actions::PathToUtf8(selected)}});
        const auto expectedOutput = fact + "sha256:" + audit::ContentDigest(fact) + "\n";
        Check(loaded.at("succeeded") == true, "Reading the listing-selected file failed: " + loaded.dump());
        Check(loaded.at("output") == expectedOutput, "Reading the listing-selected file returned unexpected output: " + loaded.dump());
        Check(loaded.at("source") == actions::PathToUtf8(selected),
            "Reading the listing-selected file changed its source identity: " + loaded.dump());
        Check(loaded.at("contentDigest") == audit::ContentDigest(fact),
            "Reading the listing-selected file lost its native content digest: " + loaded.dump());
    }
    const auto missing = firstDirectory / "missing.conf";
    const auto missingReceipt = receiptFor({{"action", "read_text_file"}, {"source", actions::PathToUtf8(missing)}});
    Check(missingReceipt.at("succeeded") == false && missingReceipt.at("attempted") == false &&
              missingReceipt.at("source") == actions::PathToUtf8(missing) && missingReceipt.at("output") == "",
        "A missing-file receipt lost its requested path or became successful content.");
    const auto recoveryDirectory = actions::Utf8ToPath(missingReceipt.at("source").get<std::string>()).parent_path();
    const auto recoveryListing = receiptFor({{"action", "list_directory"}, {"source", actions::PathToUtf8(recoveryDirectory)}});
    const auto recoveryFile = actions::Utf8ToPath(recoveryListing.at("source").get<std::string>()) / "shared.conf";
    const auto recovered = receiptFor({{"action", "read_text_file"}, {"source", actions::PathToUtf8(recoveryFile)}});
    const std::string recoveryFact = "FIRST_DIRECTORY_FACT";
    const auto expectedRecoveryOutput = recoveryFact + "sha256:" + audit::ContentDigest(recoveryFact) + "\n";
    Check(recoveryListing.at("succeeded") == true && recovered.at("succeeded") == true && recovered.at("output") == expectedRecoveryOutput,
        "Missing-read recovery could not use a real directory receipt to select the existing file: " + recovered.dump());
    tests::ScopedTestDirectory outside;
    const auto outsideFile = outside.root / "outside.conf";
    std::ofstream(outsideFile) << "OUTSIDE_SCOPE_FACT";
    const auto refused = receiptFor({{"action", "read_text_file"}, {"source", actions::PathToUtf8(outsideFile)}});
    Check(refused.at("succeeded") == false && refused.at("attempted") == false && refused.at("output") == "" &&
              refused.at("source") == actions::PathToUtf8(outsideFile),
        "Adding receipt path identity widened the captured read scope or disclosed outside content.");
    const nlohmann::json process = {{"action", "execute_process"}, {"executable", actions::PathToUtf8(executable)},
        {"working_directory", root}, {"arguments", {"cwd"}}, {"timeout_ms", 1000}};
    std::optional<actions::ActionRequest> processRequest;
    Check(agents::ParseWorkerToolResponse(nlohmann::json{{"tool", process}}.dump(), false, true, processRequest, error) && processRequest,
        error);
    auto unattendedSettings = captured.Settings();
    unattendedSettings.mode = actions::ExecutionMode::ApprovedScope;
    const auto blocked = runtime.ExecuteScopedFor(child, *processRequest, policy::CapabilityPolicy(unattendedSettings), true);
    Check(blocked.policy.verdict == actions::PolicyVerdict::Blocked && !blocked.result.attempted,
        "Explicit process delegation widened an unattended captured risk ceiling.");
    const auto unconfirmed = runtime.ExecuteScopedFor(child, *processRequest, captured, false);
    Check(!unconfirmed.Succeeded() && !unconfirmed.result.attempted,
        "A supervised process executed without explicit delegated confirmation.");
    const auto run = execute(process);
    Check(run.Succeeded() && run.result.process && run.result.process->authorityStamp.taskId == child.taskId,
        "Worker process did not preserve captured parent/attempt authority: " + run.Message());
    authority->EndTask(parent);
    Check(!execute(write).Succeeded() && !execute(process).Succeeded(), "Retired parent authority admitted worker effects.");
    const runtime::RuntimeStamp other{"other-companion", "other-session", 1, {}, {}, 1};
    runtime.BindAuthority(authority, other);
    Check(!execute({{"action", "read_text_file"}, {"source", actions::PathToUtf8(file)}}).Succeeded(),
        "A worker result crossed companion session bindings.");
}
