#include "Actions/IActionExecutor.h"
#include "Actions/actionDispatcher.h"
#include "Actions/actionTypes.h"
#include "Agents/inputArbiter.h"
#include "Agents/memoryAgent.h"
#include "Agents/conversationStylePolicy.h"
#include "Agents/replyFragmenter.h"
#include "Core/localApiKey.h"
#include "Core/conversationContext.h"
#include "Audit/actionAuditLogger.h"
#include "Filesystem/fileSystemExecutor.h"
#include "Goals/goalRunner.h"
#include "Goals/goalSandbox.h"
#include "Goals/goalStore.h"
#include "Initiative/attentionPolicy.h"
#include "Initiative/conversationStarter.h"
#include "Initiative/initiativeController.h"
#include "Goals/goalTypes.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "LLM/inferenceScheduler.h"
#include "LLM/promptBuilder.h"
#include "Learning/learningReview.h"
#include "Memory/longTermMemory.h"
#include "Perception/activityHistory.h"
#include "Perception/windowEventMonitor.h"
#include "Planning/goalPlanner.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/desktopActionRateLimiter.h"
#include "Policy/permissionStore.h"
#include "Runtime/affectController.h"
#include "Runtime/reviaSession.h"
#include "Runtime/runtimeEvents.h"
#include "Speech/speechService.h"
#include "Speech/speechRecognitionService.h"
#include "Speech/voiceActivityMonitor.h"
#include "Speech/qwenTtsClient.h"
#include "Speech/voicePresetStore.h"
#include "Windows/windowsAutomationExecutor.h"
#include "Windows/visionUiaResolver.h"
#include "Vision/visionActionParser.h"

#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <fstream>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// Win32's generic-text aliases collide with scoped ActionType members below.
#undef CopyFile
#undef CreateDirectory
#undef MoveFile
#endif

namespace
{

using revia::actions::ActionRequest;
using revia::actions::ActionType;
using revia::actions::CapabilitySettings;
using revia::actions::ExecutionMode;
using revia::actions::PolicyVerdict;
using revia::actions::RiskLevel;

class TestFailure final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw TestFailure(message);
    }
}

class ScopedTestDirectory
{
public:
    ScopedTestDirectory()
    {
        root = std::filesystem::temp_directory_path() /
            ("revia-foundation-tests-" + revia::actions::NewActionId());
        std::filesystem::create_directories(root);
    }

    ~ScopedTestDirectory()
    {
        std::error_code error;
        const std::string filename = root.filename().string();
        if (filename.rfind("revia-foundation-tests-", 0) == 0 &&
            root.parent_path() == std::filesystem::temp_directory_path())
        {
            std::filesystem::remove_all(root, error);
        }
    }

    std::filesystem::path root;
};

CapabilitySettings SupervisedSettings(const std::filesystem::path& root)
{
    CapabilitySettings settings;
    settings.mode = ExecutionMode::Supervised;
    settings.approvedRoots = {root};
    settings.autoApproveRiskThrough = RiskLevel::ReadOnly;
    settings.createMissingApprovedRoots = false;
    settings.maxReadBytes = 32;
    settings.maxDirectoryEntries = 20;
    settings.maxAffectedEntries = 20;
    return settings;
}

ActionRequest Request(
    ActionType type,
    const std::filesystem::path& source,
    const std::filesystem::path& destination = {})
{
    ActionRequest request;
    request.id = revia::actions::NewActionId();
    request.type = type;
    request.source = source;
    request.destination = destination;
    return request;
}

void WriteBytes(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream file(path, std::ios::binary);
    Check(file.is_open(), "Could not create a test file.");
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    Check(file.good(), "Could not write a test file.");
}

void TestPolicyBoundaries()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "approved";
    const auto outside = temporary.root / "outside";
    std::filesystem::create_directories(approved);
    std::filesystem::create_directories(outside);

    revia::policy::CapabilityPolicy policy(SupervisedSettings(approved));

    const auto read = policy.Evaluate(Request(ActionType::ReadTextFile, approved / "note.txt"));
    Check(read.verdict == PolicyVerdict::Allowed, "Read-only action inside the root was not allowed.");

    const auto write = policy.Evaluate(Request(ActionType::CreateDirectory, approved / "new"));
    Check(write.verdict == PolicyVerdict::RequiresConfirmation,
        "Supervised write did not require confirmation.");

    const auto escaped = policy.Evaluate(Request(
        ActionType::ReadTextFile,
        approved / ".." / "outside" / "secret.txt"));
    Check(escaped.verdict == PolicyVerdict::Blocked, "Parent traversal escaped an approved root.");

    const auto destinationEscape = policy.Evaluate(Request(
        ActionType::CopyFile,
        approved / "note.txt",
        outside / "copied.txt"));
    Check(destinationEscape.verdict == PolicyVerdict::Blocked,
        "An out-of-scope destination was accepted.");

    auto dryRunRequest = Request(ActionType::CreateDirectory, approved / "dry-run");
    dryRunRequest.dryRun = true;
    const auto dryRun = policy.Evaluate(dryRunRequest);
    Check(dryRun.verdict == PolicyVerdict::Allowed, "In-scope dry-run was not allowed.");

    auto autonomousSettings = SupervisedSettings(approved);
    autonomousSettings.mode = ExecutionMode::ApprovedScope;
    autonomousSettings.autoApproveRiskThrough = RiskLevel::ReversibleWrite;
    revia::policy::CapabilityPolicy autonomousPolicy(std::move(autonomousSettings));
    const auto autonomousWrite = autonomousPolicy.Evaluate(
        Request(ActionType::CreateDirectory, approved / "autonomous"));
    Check(autonomousWrite.verdict == PolicyVerdict::Allowed,
        "An approved-scope write below the configured ceiling was not allowed.");
}

void TestParser()
{
    revia::planning::StructuredActionParser parser;

    const auto command = parser.ParseCommand(
        R"(/copy "C:\Test Area\one.txt" "C:\Test Area\two.txt")");
    Check(command.recognized && command.succeeded, "Quoted direct command was not parsed.");
    Check(command.request.type == ActionType::CopyFile, "Direct command produced the wrong action.");
    Check(command.request.source == std::filesystem::path(R"(C:\Test Area\one.txt)"),
        "Windows source path was changed by parsing.");

    const auto proposal = parser.ParseJson(
        R"(```json
{"action":"create_directory","path":"C:\\Safe\\Folder","dry_run":true}
```)"
    );
    Check(proposal.succeeded, "Fenced structured action was not parsed.");
    Check(proposal.request.type == ActionType::CreateDirectory && proposal.request.dryRun,
        "Structured action fields were not preserved.");

    const auto shell = parser.ParseJson(
        R"({"action":"run_shell","path":"C:\\Safe"})");
    Check(!shell.succeeded, "An unsupported shell action was accepted.");

    const auto desktop = parser.ParseJson(
        R"({"action":"set_control_text","application":"notepad.exe","window_title":"Untitled","control":"Text editor","value":"hello"})");
    Check(desktop.succeeded && desktop.request.type == ActionType::SetControlText &&
        desktop.request.application == "notepad.exe" && desktop.request.value == "hello",
        "A typed desktop action was not parsed without a filesystem path.");
}

void TestDesktopActionPolicy()
{
    ScopedTestDirectory temporary;
    auto settings = SupervisedSettings(temporary.root);
    settings.approvedApplications = {"notepad.exe"};
    settings.approvedControls["notepad.exe"] = {"Text editor", "Save"};
    revia::policy::CapabilityPolicy policy(std::move(settings));

    ActionRequest inspect;
    inspect.type = ActionType::InspectWindow;
    inspect.application = "NOTEPAD.EXE";
    Check(policy.Evaluate(inspect).verdict == PolicyVerdict::Allowed,
        "Read-only inspection of an allowed application was not approved.");

    ActionRequest setText;
    setText.type = ActionType::SetControlText;
    setText.application = "notepad.exe";
    setText.control = "Text editor";
    setText.value = "hello";
    Check(policy.Evaluate(setText).verdict == PolicyVerdict::RequiresConfirmation,
        "A desktop write did not require supervised confirmation.");

    setText.control = "Delete account";
    Check(policy.Evaluate(setText).verdict == PolicyVerdict::Blocked,
        "A mutable control escaped its per-application control scope.");

    inspect.application = "powershell.exe";
    Check(policy.Evaluate(inspect).verdict == PolicyVerdict::Blocked,
        "A desktop action escaped the approved application list.");
}

void TestConfigurationFailsClosed()
{
    ScopedTestDirectory temporary;
    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        R"({"mode":"approved_scope","approvedRoots":["C:\\Safe"],"autoApproveRiskThrough":"anything"})");

    revia::policy::PermissionStore store;
    CapabilitySettings settings;
    std::string error;
    Check(!store.Load(configPath, settings, error),
        "Unknown automatic risk ceiling did not fail closed.");

    WriteBytes(configPath,
        R"({"mode":"supervised","approvedRoots":["C:\\Safe"],"approvedApplications":["notepad.exe"],"autoApproveRiskThrough":"read_only"})");
    Check(!store.Load(configPath, settings, error) &&
        error.find("approvedControls") != std::string::npos,
        "An approved application without a control scope did not fail closed.");
}

void TestDesktopActionRateLimitsAreDeterministic()
{
    revia::policy::DesktopActionRateLimiter limiter;
    limiter.Configure(2, 250);
    ActionRequest request;
    request.type = ActionType::InvokeControl;
    request.application = "notepad.exe";
    request.control = "Save";
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::seconds(10);
    std::string reason;
    Check(limiter.Admit(request, start, reason),
        "The first desktop action was rate limited.");
    Check(!limiter.Admit(request, start + std::chrono::milliseconds(100), reason) &&
        reason.find("quickly") != std::string::npos,
        "The minimum desktop-action interval was not enforced.");
    Check(limiter.Admit(request, start + std::chrono::milliseconds(300), reason),
        "A spaced desktop action was incorrectly refused.");
    Check(!limiter.Admit(request, start + std::chrono::milliseconds(600), reason) &&
        reason.find("per-minute") != std::string::npos,
        "The rolling desktop-action budget was not enforced.");

    ActionRequest inspection = request;
    inspection.type = ActionType::InspectWindow;
    Check(limiter.Admit(inspection, start + std::chrono::milliseconds(601), reason),
        "Read-only inspection consumed the mutable desktop-action budget.");
}

void TestDesktopRateLimitIsAudited()
{
    ScopedTestDirectory temporary;
    const auto configPath = temporary.root / "capabilities.json";
    const auto auditPath = temporary.root / "actions.jsonl";
    WriteBytes(configPath, nlohmann::json{
        {"mode", "supervised"},
        {"approvedRoots", nlohmann::json::array({
            revia::actions::PathToUtf8(temporary.root)})},
        {"approvedApplications", nlohmann::json::array({"revia-rate-fixture.exe"})},
        {"approvedControls", {{
            "revia-rate-fixture.exe", nlohmann::json::array({"Save"})}}},
        {"autoApproveRiskThrough", "read_only"},
        {"createMissingApprovedRoots", false},
        {"maxDesktopActionsPerMinute", 1},
        {"minimumDesktopActionIntervalMs", 0}
    }.dump());
    revia::actions::ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configPath, auditPath, error),
        "Rate-limit audit fixture did not initialize: " + error);
    ActionRequest request;
    request.id = "rate-one";
    request.type = ActionType::InvokeControl;
    request.application = "revia-rate-fixture.exe";
    request.control = "Save";
    const auto first = runtime.Execute(request, true);
    Check(first.policy.verdict == PolicyVerdict::RequiresConfirmation,
        "The first admitted desktop action did not reach ordinary policy.");
    request.id = "rate-two";
    const auto refused = runtime.Execute(request, true);
    Check(refused.policy.verdict == PolicyVerdict::Blocked && !refused.result.attempted &&
        refused.policy.reason.find("per-minute") != std::string::npos,
        "The action runtime did not convert a rate refusal into a blocked outcome.");

    std::ifstream audit(auditPath);
    std::string line;
    std::getline(audit, line);
    std::getline(audit, line);
    const auto entry = nlohmann::json::parse(line);
    Check(entry.at("policy_verdict") == "blocked" &&
        entry.at("policy_reason").get<std::string>().find("per-minute") != std::string::npos,
        "The desktop rate refusal was not visible in the action audit.");
}

void TestLlamaServerProcessRejectsMissingFiles()
{
    llmSettings settings;
    settings.serverExecutable = "missing-llama-server.exe";
    settings.modelPath = "missing-model.gguf";

    llamaCppServerProcess process;
    std::string error;
    Check(!process.Start(settings, error),
        "The llama.cpp process owner accepted a missing executable and model.");
    Check(!error.empty(), "The llama.cpp process owner did not explain its startup failure.");
    Check(!process.IsRunning() && !process.WasStartedByRevia(),
        "A rejected llama.cpp launch was incorrectly marked as owned or running.");
}

void TestLlamaServerProcessStopIsBounded()
{
#ifdef _WIN32
    ScopedTestDirectory temporary;
    const auto placeholderModel = temporary.root / "placeholder.gguf";
    WriteBytes(placeholderModel, "test");

    llmSettings settings;
    settings.serverExecutable = R"(C:\Windows\System32\ping.exe)";
    settings.modelPath = placeholderModel.string();
    settings.host = "127.0.0.1";
    settings.port = 65534;
    settings.bShutdownServerOnExit = true;

    llamaCppServerProcess process;
    std::string error;
    // The call must complete before the failure message is built. C++ does not
    // specify the evaluation order of function arguments, and GCC evaluates
    // them right to left, so passing Start(...) and "..." + error to Check in
    // one expression formats the message while error is still empty and
    // discards the actual reason for the failure.
    const bool bStarted = process.Start(settings, error);
    Check(bStarted,
        "The Windows child-process lifecycle test could not start: " + error);
    const auto start = std::chrono::steady_clock::now();
    process.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Check(elapsed < std::chrono::seconds(2),
        "Stopping an owned child process exceeded the shutdown bound.");
    Check(!process.IsRunning() && !process.WasStartedByRevia(),
        "The stopped child process was still marked as owned or running.");
#endif
}

void TestStructuredLongTermMemory()
{
    ScopedTestDirectory temporary;
    const auto memoryPath = temporary.root / "Memory" / "revia_memory.db";
    longTermMemory store(memoryPath.string());

    memoryDecision preference;
    preference.bSuccess = true;
    preference.bShouldRemember = true;
    preference.category = "preference";
    preference.summary = "The user prefers concise technical explanations.";
    preference.embeddingModel = "test-embedding-model";
    preference.embedding = {0.0f, 1.0f, 0.0f};

    bool wasAdded = false;
    Check(store.Save(preference, wasAdded) && wasAdded,
        "A valid structured memory was not added.");

    wasAdded = true;
    Check(store.Save(preference, wasAdded) && !wasAdded,
        "An identical structured memory was added twice.");

    memoryDecision vagueDuplicate = preference;
    vagueDuplicate.summary = "The user prefers explanations.";
    wasAdded = true;
    Check(store.Save(vagueDuplicate, wasAdded) && !wasAdded,
        "A vaguer restatement of a structured memory was added twice.");

    const std::vector<memoryEntry> entries = store.Load();
    Check(entries.size() == 1, "Structured memory did not load exactly one saved entry.");
    Check(entries[0].category == "preference" && entries[0].summary == preference.summary,
        "Structured memory did not preserve its category and summary.");

    memoryDecision project = preference;
    project.category = "project";
    project.summary = "The user is building Revia in C++ as a long-term project.";
    project.embedding = {1.0f, 0.0f, 0.0f};
    wasAdded = false;
    Check(store.Save(project, wasAdded) && wasAdded,
        "A second structured memory was not added to the retrieval index.");

    const std::vector<memoryEntry> relevant = store.Search("Which long-term Revia project uses C++?", 1);
    Check(relevant.size() == 1 && relevant[0].summary == project.summary,
        "Indexed memory search did not retrieve the relevant project fact.");

    const std::vector<memoryEntry> semantic = store.Search(
        "What software am I making?",
        1,
        {1.0f, 0.0f, 0.0f},
        "test-embedding-model");
    Check(semantic.size() == 1 && semantic[0].summary == project.summary,
        "Semantic memory did not retrieve a paraphrased project question.");

    const std::string promptBlock = store.BuildPromptBlock("Which explanations does the user prefer?");
    Check(promptBlock.find("[preference]") != std::string::npos &&
        promptBlock.find(preference.summary) != std::string::npos,
        "Relevant structured memory was not included in the retrieved prompt block.");

    Check(store.Load().size() == 2,
        "SQLite memory did not preserve both structured records.");
    Check(store.LoadMissingEmbeddings("test-embedding-model").empty(),
        "Saved memory vectors were not persisted for restart-safe retrieval.");

    longTermMemory reopened(memoryPath.string());
    const std::vector<memoryEntry> afterRestart = reopened.Search(
        "What software am I making?",
        1,
        {1.0f, 0.0f, 0.0f},
        "test-embedding-model");
    Check(afterRestart.size() == 1 && afterRestart[0].summary == project.summary,
        "Semantic memory vectors were not usable after reopening the database.");
}

void TestLegacyMemoryMigration()
{
    ScopedTestDirectory temporary;
    const auto memoryDirectory = temporary.root / "Memory";
    std::filesystem::create_directories(memoryDirectory);
    const auto legacyPath = memoryDirectory / "revia_memory.jsonl";
    WriteBytes(legacyPath,
        R"({"id":"legacy-1","category":"identity","summary":"The user is named Alex.","source":"automatic","createdAt":"10"})"
        "\nnot-json\n");

    longTermMemory store((memoryDirectory / "revia_memory.db").string());
    const std::vector<memoryEntry> entries = store.Load();
    Check(entries.size() == 1 && entries[0].id == "legacy-1",
        "The JSONL-to-SQLite migration did not preserve the valid legacy memory.");
    Check(store.Search("What is my name?", 1).size() == 1,
        "A migrated legacy memory was not added to the FTS retrieval index.");
    Check(store.LoadMissingEmbeddings("test-embedding-model").size() == 1,
        "A migrated legacy memory was not queued for embedding backfill.");
}

void TestBackgroundMemoryAgentLifecycle()
{
    messageRouter router;
    llmSettings settings;
    settings.backend = "Placeholder";
    embeddingSettings embeddings;
    embeddings.bEnabled = false;
    aiProfile profile;
    router.ApplyLLMSettings(settings, embeddings, profile);

    revia::agents::MemoryAgent agent;
    agent.Submit(router, "A durable test preference.");

    std::vector<revia::agents::MemoryAgentEvent> events;
    for (int attempt = 0; attempt < 100 && events.empty(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        events = agent.DrainEvents();
    }
    Check(events.size() == 1 && events[0].decision.bSuccess,
        "The background memory agent did not publish its completed result.");

    const auto start = std::chrono::steady_clock::now();
    agent.Stop();
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
        "The idle memory agent did not stop promptly.");
}

class CountingExecutor final : public revia::actions::IActionExecutor
{
public:
    explicit CountingExecutor(int& inputCalls) : calls(inputCalls) {}

    bool Handles(ActionType) const override
    {
        return true;
    }

    revia::actions::ActionResult Execute(
        const ActionRequest&,
        const revia::actions::PolicyDecision&) override
    {
        ++calls;
        revia::actions::ActionResult result;
        result.attempted = true;
        result.succeeded = true;
        result.message = "executed";
        return result;
    }

private:
    int& calls;
};

void TestDispatcherGate()
{
    int calls = 0;
    revia::actions::ActionDispatcher dispatcher;
    dispatcher.Register(std::make_unique<CountingExecutor>(calls));
    const auto request = Request(ActionType::CreateDirectory, "unused");

    revia::actions::PolicyDecision blocked;
    blocked.verdict = PolicyVerdict::Blocked;
    Check(!dispatcher.Dispatch(request, blocked).attempted && calls == 0,
        "Blocked request reached an executor.");

    revia::actions::PolicyDecision confirmation;
    confirmation.verdict = PolicyVerdict::RequiresConfirmation;
    Check(!dispatcher.Dispatch(request, confirmation, false).attempted && calls == 0,
        "Unconfirmed request reached an executor.");
    Check(dispatcher.Dispatch(request, confirmation, true).succeeded && calls == 1,
        "Confirmed request did not reach its executor.");
}

void TestFilesystemExecutorAndAudit()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "approved";
    std::filesystem::create_directories(approved);
    WriteBytes(approved / "note.txt", "hello Revia");
    WriteBytes(approved / "binary.bin", std::string("A\0B", 3));
    WriteBytes(approved / "large.txt", std::string(40, 'x'));

    revia::policy::CapabilityPolicy policy(SupervisedSettings(approved));
    revia::filesystem::FileSystemExecutor executor(32, 20, 20);

    auto listRequest = Request(ActionType::ListDirectory, approved);
    const auto listDecision = policy.Evaluate(listRequest);
    const auto listResult = executor.Execute(listRequest, listDecision);
    Check(listResult.succeeded && listResult.entries.size() == 3,
        "Directory listing did not return the test entries.");

    auto readRequest = Request(ActionType::ReadTextFile, approved / "note.txt");
    const auto readDecision = policy.Evaluate(readRequest);
    const auto readResult = executor.Execute(readRequest, readDecision);
    Check(readResult.succeeded && readResult.content == "hello Revia",
        "Text file read did not return the original content.");

    auto binaryRequest = Request(ActionType::ReadTextFile, approved / "binary.bin");
    const auto binaryResult = executor.Execute(binaryRequest, policy.Evaluate(binaryRequest));
    Check(!binaryResult.succeeded, "Binary content was displayed as text.");

    auto largeRequest = Request(ActionType::ReadTextFile, approved / "large.txt");
    const auto largeResult = executor.Execute(largeRequest, policy.Evaluate(largeRequest));
    Check(!largeResult.succeeded, "Oversized content bypassed the read limit.");

    auto dryRun = Request(ActionType::CreateDirectory, approved / "not-created");
    dryRun.dryRun = true;
    const auto dryDecision = policy.Evaluate(dryRun);
    const auto dryResult = executor.Execute(dryRun, dryDecision);
    Check(dryResult.succeeded && dryResult.dryRun &&
        !std::filesystem::exists(dryRun.source),
        "Create-directory dry-run modified the filesystem.");

    const auto auditPath = temporary.root / "audit" / "actions.jsonl";
    revia::audit::ActionAuditLogger audit(auditPath);
    Check(audit.Record(readRequest, readDecision, readResult), "Audit record was not written.");
    std::ifstream auditFile(auditPath);
    std::string line;
    std::getline(auditFile, line);
    const auto entry = nlohmann::json::parse(line);
    Check(entry.at("action") == "read_text_file" && entry.at("succeeded") == true,
        "Audit record did not preserve action outcome.");

    ActionRequest resolved;
    resolved.id = "vision-action-test";
    resolved.type = ActionType::InvokeControl;
    resolved.application = "notepad.exe";
    resolved.windowTitle = "note.txt - Notepad";
    resolved.control = "Save";
    resolved.resolution.visionResolved = true;
    resolved.resolution.modelTarget = "Save";
    resolved.resolution.regionLeft = 100;
    resolved.resolution.regionTop = 20;
    resolved.resolution.regionRight = 160;
    resolved.resolution.regionBottom = 50;
    resolved.resolution.resolvedName = "Save";
    resolved.resolution.resolvedAutomationId = "FileSave";
    resolved.resolution.resolvedRuntimeId = "42.7.9";
    resolved.resolution.matchConfidence = 0.94;
    revia::actions::PolicyDecision confirmable;
    confirmable.verdict = PolicyVerdict::RequiresConfirmation;
    revia::actions::ActionResult invoked;
    invoked.attempted = true;
    invoked.succeeded = true;
    invoked.message = "invoked";
    Check(audit.Record(resolved, confirmable, invoked),
        "Vision-resolved audit record was not written.");
    std::getline(auditFile, line);
    const auto resolvedEntry = nlohmann::json::parse(line);
    Check(resolvedEntry.contains("vision_resolution") &&
        resolvedEntry["vision_resolution"].at("resolved_runtime_id") == "42.7.9" &&
        resolvedEntry["vision_resolution"].at("match_confidence") == 0.94,
        "Audit record did not preserve vision-to-UIA resolution evidence.");
}

void TestVisionActionParserFailsClosed()
{
    revia::vision::VisionActionParser parser;
    const auto parsed = parser.Parse(
        "```json\n{\"action\":\"invoke_control\",\"target_name\":\"Save\","
        "\"target_description\":\"toolbar button\",\"region\":{\"left\":100,"
        "\"top\":20,\"right\":160,\"bottom\":52},\"value\":\"\","
        "\"confidence\":0.91}\n```");
    Check(parsed.succeeded && parsed.intent.action == ActionType::InvokeControl &&
        parsed.intent.targetName == "Save" && parsed.intent.region.right == 160,
        "A valid bounded vision action did not parse.");

    Check(!parser.Parse(
        R"({"action":"none","reason":"target is hidden"})").succeeded,
        "A vision refusal was incorrectly treated as an executable action.");
    Check(!parser.Parse(
        R"({"action":"invoke_control","target_name":"Save","region":{"left":5,"top":5,"right":5,"bottom":8},"confidence":0.9})").succeeded,
        "A zero-width vision region was accepted.");
    Check(!parser.Parse(
        R"({"action":"set_control_text","target_name":"Name","region":{"left":5,"top":5,"right":50,"bottom":30},"value":"","confidence":0.9})").succeeded,
        "An empty set-text value was accepted.");
}

void TestVisionResolverRequiresGeometryNameAndIdentity()
{
    using revia::actions::windows::VisionResolverSettings;
    using revia::actions::windows::VisionUiaResolver;
    using revia::vision::UiaCandidate;
    using revia::vision::VisionActionIntent;

    VisionActionIntent intent;
    intent.action = ActionType::InvokeControl;
    intent.targetName = "Save";
    intent.region = {100, 20, 160, 52};
    intent.modelConfidence = 0.9;

    UiaCandidate save;
    save.name = "Save";
    save.automationId = "FileSave";
    save.runtimeId = "42.7.9";
    save.bounds = {101, 21, 159, 51};
    save.enabled = true;
    save.offscreen = false;
    save.supportsInvoke = true;

    UiaCandidate deleteButton = save;
    deleteButton.name = "Delete";
    deleteButton.automationId = "Delete";
    deleteButton.runtimeId = "42.7.10";

    UiaCandidate distantSave = save;
    distantSave.runtimeId = "42.7.11";
    distantSave.bounds = {500, 500, 560, 532};

    VisionResolverSettings settings;
    const auto resolved = VisionUiaResolver::SelectBest(
        "notepad.exe",
        "note.txt - Notepad",
        intent,
        {deleteButton, distantSave, save},
        settings);
    Check(resolved.succeeded && resolved.reference.element.runtimeId == "42.7.9" &&
        resolved.reference.score.nameAgreement == 1.0,
        "Resolver did not require both spatial and accessible-name agreement.");

    UiaCandidate ambiguous = save;
    ambiguous.runtimeId = "42.7.12";
    const auto refusedAmbiguity = VisionUiaResolver::SelectBest(
        "notepad.exe",
        "note.txt - Notepad",
        intent,
        {save, ambiguous},
        settings);
    Check(!refusedAmbiguity.succeeded &&
        refusedAmbiguity.reason.find("ambiguous") != std::string::npos,
        "Two indistinguishable UIA elements did not fail closed.");

    save.runtimeId.clear();
    const auto refusedUntyped = VisionUiaResolver::SelectBest(
        "notepad.exe",
        "note.txt - Notepad",
        intent,
        {save},
        settings);
    Check(!refusedUntyped.succeeded,
        "A candidate without a stable UIA runtime id was accepted.");
}

#ifdef _WIN32
std::atomic<int>* UiaFixtureInvocations = nullptr;

LRESULT CALLBACK UiaFixtureWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM word,
    const LPARAM parameter)
{
    if (message == WM_COMMAND && HIWORD(word) == BN_CLICKED && UiaFixtureInvocations != nullptr)
    {
        UiaFixtureInvocations->fetch_add(1);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word, parameter);
}

void RunVisionUiaLiveFixture()
{
    struct Fixture
    {
        std::mutex mutex;
        std::condition_variable ready;
        HWND window = nullptr;
        HWND button = nullptr;
        bool created = false;
        std::atomic<int> invocations = 0;
    } fixture;
    UiaFixtureInvocations = &fixture.invocations;

    std::jthread windowThread([&fixture]()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = UiaFixtureWindowProcedure;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"ReviaUiaResolverFixture";
        RegisterClassW(&windowClass);
        const HWND window = CreateWindowExW(
            0,
            windowClass.lpszClassName,
            L"Revia UIA Fixture",
            WS_OVERLAPPEDWINDOW,
            200,
            200,
            420,
            240,
            nullptr,
            nullptr,
            instance,
            nullptr);
        const HWND button = window == nullptr ? nullptr : CreateWindowExW(
            0,
            L"BUTTON",
            L"Save",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            120,
            80,
            100,
            36,
            window,
            reinterpret_cast<HMENU>(1001),
            instance,
            nullptr);
        if (window != nullptr)
        {
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);
        }
        {
            std::lock_guard lock(fixture.mutex);
            fixture.window = window;
            fixture.button = button;
            fixture.created = window != nullptr && button != nullptr;
        }
        fixture.ready.notify_one();
        MSG message{};
        while (window != nullptr && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        UnregisterClassW(windowClass.lpszClassName, instance);
    });

    const auto stopFixture = [&fixture, &windowThread]()
    {
        if (fixture.window != nullptr)
        {
            PostMessageW(fixture.window, WM_CLOSE, 0, 0);
        }
        if (windowThread.joinable())
        {
            windowThread.join();
        }
        UiaFixtureInvocations = nullptr;
    };

    try
    {
      {
        std::unique_lock lock(fixture.mutex);
        fixture.ready.wait_for(lock, std::chrono::seconds(3), [&fixture]()
        {
            return fixture.created;
        });
      }
    Check(fixture.created, "The live UIA fixture window could not be created.");

    RECT bounds{};
    Check(GetWindowRect(fixture.button, &bounds),
        "The live UIA fixture button bounds were unavailable.");
    revia::vision::VisionActionIntent intent;
    intent.action = ActionType::InvokeControl;
    intent.targetName = "Save";
    intent.region = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    intent.modelConfidence = 0.99;
    revia::actions::windows::VisionUiaResolver resolver;
    const auto resolved = resolver.Resolve(
        "ReviaTests.exe",
        "Revia UIA Fixture",
        intent,
        {});
    Check(resolved.succeeded && !resolved.reference.element.runtimeId.empty(),
        "Live vision-to-UIA resolution failed: " + resolved.reason);

    ActionRequest request;
    request.type = ActionType::InvokeControl;
    request.application = resolved.reference.application;
    request.windowTitle = resolved.reference.windowTitle;
    request.control = resolved.reference.element.name;
    request.resolution.visionResolved = true;
    request.resolution.resolvedName = resolved.reference.element.name;
    request.resolution.resolvedAutomationId = resolved.reference.element.automationId;
    request.resolution.resolvedRuntimeId = resolved.reference.element.runtimeId;
    request.resolution.resolvedControlType = resolved.reference.element.controlType;
    revia::actions::PolicyDecision allowed;
    allowed.verdict = PolicyVerdict::Allowed;
    revia::actions::windows::WindowsAutomationExecutor executor;
    const auto invoked = executor.Execute(request, allowed);
    for (int attempt = 0; attempt < 100 && fixture.invocations.load() == 0; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(invoked.succeeded && fixture.invocations.load() == 1,
        "The exact live UIA element was not invoked.");

    SetWindowTextW(fixture.button, L"Changed after resolution");
    const auto stale = executor.Execute(request, allowed);
    Check(!stale.succeeded && fixture.invocations.load() == 1,
        "A changed UIA element bypassed the typed-identity recheck.");
    }
    catch (...)
    {
        stopFixture();
        throw;
    }
    stopFixture();
}

std::wstring ProcessExecutableName(const DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
    {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!queried)
    {
        return {};
    }
    path.resize(length);
    return std::filesystem::path(path).filename().wstring();
}

BOOL CALLBACK CollectNotepadWindows(const HWND window, const LPARAM parameter)
{
    if (!IsWindowVisible(window))
    {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    std::wstring executable = ProcessExecutableName(processId);
    std::transform(executable.begin(), executable.end(), executable.begin(), ::towlower);
    if (executable == L"notepad.exe")
    {
        reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
    }
    return TRUE;
}

void RunVisionActionNotepadLive()
{
    revia::runtime::ReviaSession session;
    std::vector<HWND> existingNotepadWindows;
    EnumWindows(
        CollectNotepadWindows,
        reinterpret_cast<LPARAM>(&existingNotepadWindows));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"C:\\Windows\\System32\\notepad.exe /new";
    const bool launched = CreateProcessW(
        nullptr,
        command.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startup,
        &process) != FALSE;
    if (launched)
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }

    HWND notepad = nullptr;
    for (int attempt = 0; attempt < 100 && notepad == nullptr; ++attempt)
    {
        std::vector<HWND> currentNotepadWindows;
        EnumWindows(
            CollectNotepadWindows,
            reinterpret_cast<LPARAM>(&currentNotepadWindows));
        const auto newWindow = std::find_if(
            currentNotepadWindows.begin(),
            currentNotepadWindows.end(),
            [&](const HWND candidate)
            {
                return std::find(
                    existingNotepadWindows.begin(),
                    existingNotepadWindows.end(),
                    candidate) == existingNotepadWindows.end();
            });
        if (newWindow != currentNotepadWindows.end())
        {
            notepad = *newWindow;
        }
        if (notepad == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    const auto cleanup = [&]()
    {
        if (notepad != nullptr)
        {
            PostMessageW(notepad, WM_CLOSE, 0, 0);
        }
        session.Stop();
    };
    try
    {
        Check(launched && notepad != nullptr,
            "A controlled Notepad window could not be opened.");
        ShowWindow(notepad, SW_MAXIMIZE);
        SetWindowPos(
            notepad,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(
            notepad,
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        MSG queuedMessage{};
        PeekMessageW(&queuedMessage, nullptr, 0, 0, PM_NOREMOVE);
        const DWORD currentThread = GetCurrentThreadId();
        const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD notepadThread = GetWindowThreadProcessId(notepad, nullptr);
        if (foregroundThread != 0 && foregroundThread != currentThread)
        {
            AttachThreadInput(currentThread, foregroundThread, TRUE);
        }
        if (notepadThread != 0 && notepadThread != currentThread)
        {
            AttachThreadInput(currentThread, notepadThread, TRUE);
        }
        BringWindowToTop(notepad);
        SetForegroundWindow(notepad);
        SetFocus(notepad);
        if (notepadThread != 0 && notepadThread != currentThread)
        {
            AttachThreadInput(currentThread, notepadThread, FALSE);
        }
        if (foregroundThread != 0 && foregroundThread != currentThread)
        {
            AttachThreadInput(currentThread, foregroundThread, FALSE);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        if (GetForegroundWindow() != notepad)
        {
            cleanup();
            std::cout << "Vision-action Notepad fixture skipped: Windows did not grant "
                "the background test process foreground focus.\n";
            return;
        }
        Check(session.Start(),
            "ReviaSession could not start for the live Notepad vision test.");
        session.SetConfirmationHandler([](
            const revia::actions::ActionRequest&,
            const revia::actions::PolicyDecision&)
        {
            return true;
        });
        if (GetForegroundWindow() != notepad)
        {
            cleanup();
            std::cout << "Vision-action Notepad fixture skipped: foreground focus changed "
                "while the local model started.\n";
            return;
        }
        const revia::runtime::SessionResult action = session.ActOnScreen(
            "Open Notepad's File menu by pressing the visible File menu control.");
        Check(action.succeeded,
            "The end-to-end Notepad screen action failed: " + action.reason);
    }
    catch (...)
    {
        cleanup();
        throw;
    }
    cleanup();
    std::cout << "Vision-grounded Notepad action passed.\n";
}
#endif

void TestRuntimeEventBus()
{
    revia::runtime::RuntimeEventBus bus;
    int firstCalls = 0;
    int secondCalls = 0;
    revia::runtime::RuntimeEvent received;

    const auto first = bus.Subscribe([&](const revia::runtime::RuntimeEvent& event)
    {
        ++firstCalls;
        received = event;
    });
    const auto second = bus.Subscribe([&](const revia::runtime::RuntimeEvent&)
    {
        ++secondCalls;
    });

    bus.Publish({
        revia::runtime::RuntimeEventKind::StateChanged,
        revia::runtime::RuntimeState::Thinking,
        "Thinking about a test turn.",
        7});
    Check(firstCalls == 1 && secondCalls == 1,
        "Runtime event bus did not notify every listener.");
    Check(received.state == revia::runtime::RuntimeState::Thinking &&
        received.turnId == 7 &&
        revia::runtime::ToString(received.state) == "Thinking",
        "Runtime event payload did not preserve state or turn identity.");

    bus.Unsubscribe(first);
    bus.Publish({
        revia::runtime::RuntimeEventKind::Activity,
        revia::runtime::RuntimeState::Idle,
        "Idle"});
    Check(firstCalls == 1 && secondCalls == 2,
        "Runtime event bus did not remove the selected listener.");
    bus.Unsubscribe(second);
}

void TestAffectController()
{
    using namespace std::chrono_literals;
    revia::runtime::AffectController controller(0ms, 5ms);

    const auto curious = controller.ObserveTurn(
        "Why did that happen?", "Here is what happened.", true);
    Check(curious.state == revia::runtime::AffectState::Curious,
        "A question did not produce a curious response posture.");

    const auto focused = controller.ObserveTurn(
        "Fix this broken build", "I found the failure.", true);
    Check(focused.state == revia::runtime::AffectState::Focused && focused.intensity > 0.75F,
        "A concrete problem did not produce a focused response posture.");

    const auto concerned = controller.ObserveTurn(
        "Run it", "The operation failed.", false);
    Check(concerned.state == revia::runtime::AffectState::Concerned,
        "A failed operation did not produce a concerned response posture.");

    std::this_thread::sleep_for(10ms);
    const auto decayed = controller.Tick();
    Check(decayed.has_value() && decayed->state == revia::runtime::AffectState::Neutral,
        "Affect did not return to its neutral baseline after the decay interval.");
}

void TestSpeechTextNormalization()
{
    const std::string normalized = revia::speech::SpeechService::NormalizeForSpeech(
        "**Hello** [there](https://example.com).\n```cpp\nint hidden = 1;\n```\n"
        "Read https://example.com later.", 500);
    Check(normalized.find("Hello") != std::string::npos &&
        normalized.find("hidden") == std::string::npos &&
        normalized.find("https://") == std::string::npos,
        "Speech normalization leaked markdown code or a raw URL.");

    const std::string clipped = revia::speech::SpeechService::NormalizeForSpeech(
        "one two three four five six seven eight nine ten", 20);
    Check(clipped.size() <= 21 && clipped.ends_with('.'),
        "Speech normalization did not bound long utterances.");
}

void TestSpeechRuntimePathResolution()
{
    const std::filesystem::path resolved =
        revia::speech::SpeechRecognitionService::ResolveRuntimePath("Config/settings.json");
    Check(std::filesystem::is_regular_file(resolved),
        "Speech recognition could not resolve a repository-relative runtime path.");
}

void TestVoicePresetPersistence()
{
    ScopedTestDirectory temporary;
    const std::filesystem::path voiceRoot = temporary.root / "voices";
    const std::filesystem::path reference = voiceRoot / "revia-bright" / "reference.wav";
    std::filesystem::create_directories(reference.parent_path());
    WriteBytes(reference, "RIFF-test-WAVE");

    revia::speech::VoicePreset preset;
    preset.id = "revia-bright";
    preset.name = "Revia Bright";
    preset.description = "Bright, youthful, clear, and synthetic.";
    preset.language = "English";
    preset.referenceText = "Wait, I found the pattern.";
    preset.referenceAudioPath = reference.string();
    preset.createdAt = "2026-08-16T00:00:00Z";

    revia::speech::VoicePresetStore store(voiceRoot);
    std::string error;
    // Sequenced for the same reason as the child-process test above: the
    // message must not be built before the call that populates error.
    const bool bSaved = store.Save(preset, error);
    Check(bSaved, "Voice preset save failed: " + error);
    const bool bAssigned = store.Assign("revia", preset.id, error);
    Check(bAssigned, "Voice assignment failed: " + error);

    revia::speech::VoicePresetStore reopened(voiceRoot);
    const auto loaded = reopened.Find(preset.id);
    Check(loaded.has_value() && loaded->name == preset.name &&
        reopened.AssignedPresetId("revia") == preset.id,
        "Voice preset or profile assignment did not persist.");
    Check(reopened.Assign("revia", "", error) && reopened.AssignedPresetId("revia").empty(),
        "Windows voice fallback assignment did not persist.");
    Check(!reopened.Assign("../unsafe", preset.id, error),
        "Voice profile assignment accepted a traversal id.");
}

void TestWindowsSpeechServiceInitialization()
{
#ifdef _WIN32
    speechSettings settings;
    settings.bEnabled = false;
    std::atomic<bool> initialized = false;
    revia::speech::SpeechService service;
    service.Start(settings, [&](const revia::speech::SpeechEvent& event)
    {
        if (event.phase == "Disabled")
        {
            initialized.store(true);
        }
    });
    for (int attempt = 0; attempt < 100 && !initialized.load(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    service.Shutdown();
    Check(initialized.load(), "Windows SAPI did not initialize on the speech worker.");
#endif
}

void TestLocalApiKeys()
{
    const std::string first = revia::core::GenerateLocalApiKey();
    const std::string second = revia::core::GenerateLocalApiKey();
    Check(first.starts_with("revia-") && first.size() == 70 && first != second,
        "Per-run llama.cpp API keys were missing, malformed, or repeated.");
}


nlohmann::json CapabilityConfigJson(
    const std::string& mode,
    const std::filesystem::path& approvedRoot,
    const std::string& riskCeiling)
{
    return nlohmann::json{
        {"mode", mode},
        {"approvedRoots", nlohmann::json::array({revia::actions::PathToUtf8(approvedRoot)})},
        {"approvedApplications", nlohmann::json::array()},
        {"approvedControls", nlohmann::json::object()},
        {"autoApproveRiskThrough", riskCeiling},
        {"createMissingApprovedRoots", false},
        {"maxReadBytes", 1048576},
        {"maxDirectoryEntries", 200},
        {"maxAffectedEntries", 200},
        {"maxDesktopActionsPerMinute", 12},
        {"minimumDesktopActionIntervalMs", 250}
    };
}

CapabilitySettings GoalScope(const std::filesystem::path& approvedRoot)
{
    CapabilitySettings scope;
    scope.mode = ExecutionMode::ApprovedScope;
    scope.approvedRoots = {approvedRoot};
    scope.autoApproveRiskThrough = RiskLevel::ReversibleWrite;
    scope.createMissingApprovedRoots = false;
    return scope;
}

revia::goals::GoalStep MakeDirectoryStep(
    const std::filesystem::path& parent,
    const std::string& folderName)
{
    revia::goals::GoalStep step;
    step.description = "Create " + folderName;
    step.action = Request(ActionType::CreateDirectory, parent / folderName);
    step.check = Request(ActionType::ListDirectory, parent);
    step.expected = folderName;
    return step;
}

void TestGoalStoreRoundTrip()
{
    ScopedTestDirectory temporary;
    const revia::goals::GoalStore store((temporary.root / "goals.db").string());

    revia::goals::Goal goal;
    goal.id = revia::goals::NewGoalId();
    goal.title = "Create a Notes folder";
    goal.status = revia::goals::GoalStatus::Running;
    goal.budget.maxActions = 7;
    goal.spend.actions = 3;
    goal.scope = GoalScope(temporary.root);

    revia::goals::GoalStep step = MakeDirectoryStep(temporary.root, "Notes");
    step.id = revia::goals::NewStepId();
    step.ordinal = 0;
    step.status = revia::goals::StepStatus::Failed;

    revia::goals::StepAttempt attempt;
    attempt.attempt = 1;
    attempt.actionId = "action-1";
    attempt.checkActionId = "action-2";
    attempt.verdict = PolicyVerdict::Allowed;
    attempt.executed = true;
    attempt.verified = false;
    attempt.observation = "[DIR]   Other";
    attempt.failure = "Verification did not observe: Notes";
    step.attempts.push_back(attempt);
    goal.steps.push_back(step);

    Check(store.Save(goal), "The goal store did not save a goal.");

    const auto reloaded = store.Load(goal.id);
    Check(reloaded.has_value(), "The goal store did not reload a saved goal.");
    Check(reloaded->title == goal.title &&
        reloaded->status == revia::goals::GoalStatus::Running &&
        reloaded->budget.maxActions == 7 &&
        reloaded->spend.actions == 3,
        "Goal header fields did not survive a store round trip.");
    Check(reloaded->steps.size() == 1 &&
        reloaded->steps.front().action.type == ActionType::CreateDirectory &&
        reloaded->steps.front().check.type == ActionType::ListDirectory &&
        reloaded->steps.front().expected == "Notes",
        "Goal steps did not survive a store round trip.");
    Check(reloaded->steps.front().attempts.size() == 1 &&
        reloaded->steps.front().attempts.front().observation == "[DIR]   Other" &&
        !reloaded->steps.front().attempts.front().verified,
        "Step evidence did not survive a store round trip.");
    Check(reloaded->scope.approvedRoots.size() == 1 &&
        reloaded->scope.mode == ExecutionMode::ApprovedScope,
        "The per-goal capability scope did not survive a store round trip.");

    const auto resumable = store.LoadResumable();
    Check(resumable.size() == 1 && resumable.front().id == goal.id,
        "An unfinished goal was not offered for resume.");

    Check(store.Remove(goal.id) && !store.Load(goal.id).has_value(),
        "A removed goal was still readable.");
}

void TestGoalRunnerRejectsUnverifiablePlan()
{
    std::string error;

    revia::goals::Goal destructiveCheck;
    revia::goals::GoalStep step;
    step.action = Request(ActionType::CreateDirectory, "C:/anything");
    step.check = Request(ActionType::MoveToRecycleBin, "C:/anything");
    step.expected = "anything";
    destructiveCheck.steps.push_back(step);
    Check(!revia::goals::GoalRunner::Validate(destructiveCheck, error),
        "A plan that verifies with a destructive action was accepted.");

    revia::goals::Goal missingCheck;
    revia::goals::GoalStep bare;
    bare.action = Request(ActionType::CreateDirectory, "C:/anything");
    bare.expected = "anything";
    missingCheck.steps.push_back(bare);
    Check(!revia::goals::GoalRunner::Validate(missingCheck, error),
        "A plan with no verification action was accepted.");

    revia::goals::Goal noExpectation;
    revia::goals::GoalStep silent = MakeDirectoryStep("C:/anything", "Notes");
    silent.expected.clear();
    noExpectation.steps.push_back(silent);
    Check(!revia::goals::GoalRunner::Validate(noExpectation, error),
        "A plan that never says what success looks like was accepted.");
}

void TestGoalRunnerVerifiesSuccess()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);

    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("approved_scope", approved, "reversible_write").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    const bool initialized =
        runtime.Initialize(configPath, temporary.root / "audit.jsonl", error);
    Check(initialized, "The action runtime did not initialize for the goal runner: " + error);

    const revia::goals::GoalStore store((temporary.root / "goals.db").string());
    revia::goals::GoalRunner runner(runtime, store);

    revia::goals::Goal goal;
    goal.title = "Create the Notes folder";
    goal.scope = GoalScope(approved);
    goal.steps.push_back(MakeDirectoryStep(approved, "Notes"));

    const revia::goals::Goal finished = runner.Run(goal);
    Check(finished.status == revia::goals::GoalStatus::Succeeded &&
        finished.stopReason == revia::goals::StopReason::Completed,
        "A verifiable goal did not complete.");
    Check(std::filesystem::is_directory(approved / "Notes"),
        "The goal reported success without creating the directory.");
    Check(finished.steps.size() == 1 &&
        finished.steps.front().attempts.size() == 1 &&
        finished.steps.front().attempts.front().verified,
        "The completed step did not record verification evidence.");
    Check(finished.spend.actions == 2,
        "A verified step did not cost exactly one action and one observation.");
    Check(store.Load(finished.id).has_value(),
        "The finished goal was not persisted.");
}

void TestGoalRunnerStopsOnUnverifiableStep()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);

    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("approved_scope", approved, "reversible_write").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    const bool initialized =
        runtime.Initialize(configPath, temporary.root / "audit.jsonl", error);
    Check(initialized, "The action runtime did not initialize for the retry test: " + error);

    const revia::goals::GoalStore store((temporary.root / "goals.db").string());
    revia::goals::GoalRunner runner(runtime, store);

    revia::goals::Goal goal;
    goal.title = "Claim a folder that is never created";
    goal.scope = GoalScope(approved);
    goal.budget.maxRetriesPerStep = 1;

    revia::goals::GoalStep step = MakeDirectoryStep(approved, "Notes");
    step.expected = "AFolderThatIsNeverCreated";
    goal.steps.push_back(step);

    const revia::goals::Goal finished = runner.Run(goal);
    Check(finished.status == revia::goals::GoalStatus::Failed &&
        finished.stopReason == revia::goals::StopReason::VerificationFailed,
        "An unverified step was allowed to complete.");
    Check(finished.steps.front().attempts.size() == 2,
        "The runner did not retry an unverified step up to its per-step budget.");
    Check(finished.spend.retries == 1, "Retry spend was not recorded.");
}

void TestGoalScopeCannotWidenAuthority()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    const auto outside = temporary.root / "outside";
    std::filesystem::create_directories(approved);
    std::filesystem::create_directories(outside);

    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("approved_scope", approved, "reversible_write").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    const bool initialized =
        runtime.Initialize(configPath, temporary.root / "audit.jsonl", error);
    Check(initialized, "The action runtime did not initialize for the scope test: " + error);

    const revia::goals::GoalStore store((temporary.root / "goals.db").string());
    revia::goals::GoalRunner runner(runtime, store);

    revia::goals::Goal goal;
    goal.title = "Write outside the globally approved root";
    goal.scope = GoalScope(outside);
    goal.steps.push_back(MakeDirectoryStep(outside, "Escaped"));

    const revia::goals::Goal finished = runner.Run(goal);
    Check(finished.status == revia::goals::GoalStatus::Blocked &&
        finished.stopReason == revia::goals::StopReason::PolicyBlocked,
        "A per-goal scope widened the globally approved roots.");
    Check(!std::filesystem::exists(outside / "Escaped"),
        "A blocked goal still changed the filesystem.");
}


void TestGoalResumesAfterRestart()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);

    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("approved_scope", approved, "reversible_write").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    const bool initialized =
        runtime.Initialize(configPath, temporary.root / "audit.jsonl", error);
    Check(initialized, "The action runtime did not initialize for the resume test: " + error);

    const revia::goals::GoalStore store((temporary.root / "goals.db").string());

    // Stands in for a process that died mid-step: persisted as Running with the
    // step still marked Acting, so nothing about it was ever observed.
    revia::goals::Goal interrupted;
    interrupted.id = revia::goals::NewGoalId();
    interrupted.title = "Interrupted goal";
    interrupted.status = revia::goals::GoalStatus::Running;
    interrupted.scope = GoalScope(approved);

    revia::goals::GoalStep step = MakeDirectoryStep(approved, "Notes");
    step.id = revia::goals::NewStepId();
    step.status = revia::goals::StepStatus::Acting;
    interrupted.steps.push_back(step);
    Check(store.Save(interrupted), "The interrupted goal was not saved.");

    revia::goals::GoalRunner runner(runtime, store);
    const revia::goals::Goal resumed = runner.Resume(interrupted.id);
    Check(resumed.status == revia::goals::GoalStatus::Succeeded,
        "An interrupted goal did not resume to completion after a restart.");
    Check(std::filesystem::is_directory(approved / "Notes"),
        "The resumed goal did not retry its unobserved step.");
}

void TestGoalPlannerParsesMultiStepPlan()
{
    const std::string plan = R"({
        "title": "Set up the Notes folder",
        "steps": [
            {
                "description": "Create the folder",
                "action": {"action": "create_directory", "path": "C:/Sandbox/Notes"},
                "check": {"action": "list_directory", "path": "C:/Sandbox"},
                "expected": "Notes"
            },
            {
                "description": "Copy the template in",
                "action": {"action": "copy_file", "source": "C:/Sandbox/template.txt",
                           "destination": "C:/Sandbox/Notes/template.txt"},
                "check": {"action": "list_directory", "path": "C:/Sandbox/Notes"},
                "expected": "template.txt"
            }
        ]
    })";

    const auto parsed = revia::planning::GoalPlanner::ParseJson(plan);
    Check(parsed.succeeded, "A well-formed goal plan was rejected: " + parsed.error);
    Check(parsed.goal.title == "Set up the Notes folder",
        "The goal title did not survive planning.");
    Check(parsed.goal.steps.size() == 2, "The planner lost a step.");
    Check(!parsed.goal.id.empty() && !parsed.goal.steps.front().id.empty(),
        "Planned goals and steps must carry generated ids.");
    Check(parsed.goal.steps[0].ordinal == 0 && parsed.goal.steps[1].ordinal == 1,
        "Planned steps were not ordered.");
    Check(parsed.goal.steps[0].action.type == ActionType::CreateDirectory &&
        parsed.goal.steps[0].check.type == ActionType::ListDirectory &&
        parsed.goal.steps[0].expected == "Notes",
        "The first planned step did not decode into action, check, and expectation.");
    Check(parsed.goal.steps[1].action.type == ActionType::CopyFile &&
        !parsed.goal.steps[1].action.destination.empty(),
        "A two-path action lost its destination during planning.");

    // The parser must not invent authority. Scope stays at the struct default until the
    // session narrows it from configured policy.
    Check(parsed.goal.scope.approvedRoots.empty(),
        "A planned goal must not carry an approved root chosen by the model.");
}

void TestGoalPlannerRejectsUnusablePlans()
{
    const auto unknown = revia::planning::GoalPlanner::ParseJson(
        R"({"goal": "unknown", "reason": "needs a shell command"})");
    Check(!unknown.succeeded, "A plan the model declined to make was accepted.");

    const auto noSteps = revia::planning::GoalPlanner::ParseJson(
        R"({"title": "Nothing", "steps": []})");
    Check(!noSteps.succeeded, "A plan with no steps was accepted.");

    const auto missingCheck = revia::planning::GoalPlanner::ParseJson(
        R"({"title": "T", "steps": [{"action": {"action": "create_directory",
            "path": "C:/Sandbox/Notes"}, "expected": "Notes"}]})");
    Check(!missingCheck.succeeded, "A step with no check was accepted.");

    const auto badAction = revia::planning::GoalPlanner::ParseJson(
        R"({"title": "T", "steps": [{"action": {"action": "run_shell", "path": "C:/x"},
            "check": {"action": "list_directory", "path": "C:/x"}, "expected": "x"}]})");
    Check(!badAction.succeeded, "A step naming an action outside the allowlist was accepted.");

    const auto notJson = revia::planning::GoalPlanner::ParseJson("I'll get right on that!");
    Check(!notJson.succeeded, "Prose was accepted as a goal plan.");

    // A runaway plan has to be refused at authoring time. The goal budget would only
    // catch it once execution was already underway.
    std::string oversized = R"({"title": "Too many", "steps": [)";
    for (std::size_t index = 0; index <= revia::planning::GoalPlanner::MaximumSteps; ++index)
    {
        oversized += R"({"action": {"action": "create_directory", "path": "C:/Sandbox/A"},
            "check": {"action": "list_directory", "path": "C:/Sandbox"},
            "expected": "A"},)";
    }
    oversized.pop_back();
    oversized += "]}";
    const auto tooMany = revia::planning::GoalPlanner::ParseJson(oversized);
    Check(!tooMany.succeeded, "A plan longer than the step ceiling was accepted.");
}

void TestGoalPlannerAcceptsRealModelOutput()
{
    // Verbatim output from Qwen3-VL-8B for "Make a folder called Reports in C:/Sandbox and
    // copy C:/Sandbox/summary.txt into it". Pinned because the prompt contract and the
    // parser have to agree with what the model actually emits, not with what the prompt
    // asks for -- including its habit of escaping Windows separators.
    const std::string realOutput =
        R"({"title":"Create Reports folder and copy summary.txt","steps":[)"
        R"({"description":"Create the Reports directory in C:/Sandbox",)"
        R"("action":{"action":"create_directory","path":"C:/Sandbox/Reports"},)"
        R"("check":{"action":"list_directory","path":"C:/Sandbox"},"expected":"Reports"},)"
        R"({"description":"Copy summary.txt into the Reports folder",)"
        R"("action":{"action":"copy_file","source":"C:/Sandbox/summary.txt",)"
        R"("destination":"C:/Sandbox/Reports/summary.txt"},)"
        R"("check":{"action":"list_directory","path":"C:/Sandbox/Reports"},)"
        R"("expected":"summary.txt"}]})";

    const auto parsed = revia::planning::GoalPlanner::ParseJson(realOutput);
    Check(parsed.succeeded, "Real planner output was rejected: " + parsed.error);
    Check(parsed.goal.steps.size() == 2, "Real planner output lost a step.");
    Check(parsed.goal.steps[1].action.type == ActionType::CopyFile &&
        !parsed.goal.steps[1].action.destination.empty(),
        "Real planner output lost its copy destination.");

    std::string error;
    Check(revia::goals::GoalRunner::Validate(parsed.goal, error),
        "Real planner output failed validation: " + error);
}

void TestPlannedDestructivePlanIsContained()
{
    // Also verbatim, from the request "Clean up my desktop". The planner will propose a
    // destructive action for a vague request, so the containment has to be structural
    // rather than a matter of the model behaving well.
    const std::string realOutput =
        R"({"title":"Clean Up Desktop","steps":[{"description":"Move all files and folders )"
        R"(from desktop to recycle bin","action":{"action":"move_to_recycle_bin",)"
        R"("path":"C:\\Users\\Public\\Desktop"},"check":{"action":"list_directory",)"
        R"("path":"C:\\Users\\Public\\Desktop"},"expected":"empty"}]})";

    const auto parsed = revia::planning::GoalPlanner::ParseJson(realOutput);
    Check(parsed.succeeded, "The destructive plan should parse; containment is not the "
        "parser's job: " + parsed.error);

    // Structurally well formed, so Validate passes it. That is correct and is exactly why
    // it must not be the only gate.
    std::string error;
    Check(revia::goals::GoalRunner::Validate(parsed.goal, error),
        "This plan is structurally valid; the test's premise is wrong if it is not.");

    // Recycling is classified ReversibleWrite, not Destructive, because the bin is
    // recoverable. So the risk ceiling does NOT stop this plan, and the containment that
    // actually load-bears is the approved-root boundary. Assert that directly rather than
    // assuming the ceiling covers it.
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);

    CapabilitySettings wide;
    wide.mode = ExecutionMode::Supervised;
    wide.autoApproveRiskThrough = RiskLevel::Destructive;
    wide.approvedRoots = {approved};
    wide.createMissingApprovedRoots = false;

    const revia::policy::CapabilityPolicy scopedPolicy(
        revia::goals::NarrowScopeForGoal(wide));
    const revia::actions::PolicyDecision outsideRoot =
        scopedPolicy.Evaluate(parsed.goal.steps.front().action);
    Check(outsideRoot.verdict == PolicyVerdict::Blocked,
        "A planned goal was allowed to recycle a path outside every approved root.");

    // Inside an approved root the same action is permitted, which is the point: the plan
    // is contained by where it may act, and the whole plan is shown for approval before
    // any of it runs.
    ActionRequest insideRoot = parsed.goal.steps.front().action;
    insideRoot.source = approved / "scratch.txt";
    Check(scopedPolicy.Evaluate(insideRoot).verdict != PolicyVerdict::Blocked,
        "A goal scope refused a reversible write inside its own approved root.");
}

void TestPlannedGoalStillFacesValidation()
{
    // Authoring and execution must not disagree about what a usable step is. The parser
    // accepts a structurally complete step; Validate is what refuses a destructive check
    // or a missing expectation, and it has to still fire on a planner-produced goal.
    const auto destructiveCheck = revia::planning::GoalPlanner::ParseJson(
        R"({"title": "T", "steps": [{"action": {"action": "create_directory",
            "path": "C:/Sandbox/Notes"},
            "check": {"action": "move_to_recycle_bin", "path": "C:/Sandbox/Notes"},
            "expected": "Notes"}]})");
    Check(destructiveCheck.succeeded,
        "The parser should decode this plan and leave the verdict to Validate.");
    std::string error;
    Check(!revia::goals::GoalRunner::Validate(destructiveCheck.goal, error),
        "A planned goal that verifies with a destructive action passed validation.");

    const auto noExpectation = revia::planning::GoalPlanner::ParseJson(
        R"({"title": "T", "steps": [{"action": {"action": "create_directory",
            "path": "C:/Sandbox/Notes"},
            "check": {"action": "list_directory", "path": "C:/Sandbox"}}]})");
    Check(noExpectation.succeeded, "The parser should decode a step with no expectation.");
    Check(!revia::goals::GoalRunner::Validate(noExpectation.goal, error),
        "A planned goal that never says what success looks like passed validation.");
}

void TestPlannedGoalRunsEndToEnd()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);

    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("approved_scope", approved, "reversible_write").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configPath, temporary.root / "audit.jsonl", error),
        "The action runtime did not initialize for the planned-goal test: " + error);

    // Exactly the shape the planner emits, built against a real sandbox path.
    nlohmann::json plan;
    plan["title"] = "Create the Notes folder";
    plan["steps"] = nlohmann::json::array({{
        {"description", "Create it"},
        {"action", {{"action", "create_directory"},
            {"path", revia::actions::PathToUtf8(approved / "Notes")}}},
        {"check", {{"action", "list_directory"},
            {"path", revia::actions::PathToUtf8(approved)}}},
        {"expected", "Notes"}
    }});

    auto parsed = revia::planning::GoalPlanner::ParseJson(plan.dump());
    Check(parsed.succeeded, "The planner rejected its own plan shape: " + parsed.error);
    parsed.goal.scope = GoalScope(approved);

    const revia::goals::GoalStore store((temporary.root / "goals.db").string());
    revia::goals::GoalRunner runner(runtime, store);
    const revia::goals::Goal finished = runner.Run(parsed.goal);

    Check(finished.status == revia::goals::GoalStatus::Succeeded,
        "A planned goal did not run to completion.");
    Check(std::filesystem::is_directory(approved / "Notes"),
        "The planned goal reported success without doing the work.");
    Check(finished.steps.front().attempts.front().verified,
        "The planned goal completed without recording verification evidence.");
}

void TestSandboxRehearsalLeavesRealFoldersAlone()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);
    WriteBytes(approved / "summary.txt", "quarterly numbers");

    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("approved_scope", approved, "reversible_write").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configPath, temporary.root / "audit.jsonl", error),
        "The action runtime did not initialize for the rehearsal test: " + error);

    revia::goals::Goal goal;
    goal.id = revia::goals::NewGoalId();
    goal.title = "Create Reports and copy the summary in";
    goal.scope = GoalScope(approved);
    goal.steps.push_back(MakeDirectoryStep(approved, "Reports"));

    revia::goals::GoalStep copyStep;
    copyStep.ordinal = 1;
    copyStep.description = "Copy the summary in";
    copyStep.action = Request(ActionType::CopyFile, approved / "summary.txt");
    copyStep.action.destination = approved / "Reports" / "summary.txt";
    copyStep.check = Request(ActionType::ListDirectory, approved / "Reports");
    copyStep.expected = "summary.txt";
    goal.steps.push_back(copyStep);

    const auto rehearsal = revia::goals::GoalSandbox::Prepare(goal);
    Check(rehearsal.supported && rehearsal.prepared,
        "A filesystem plan could not be rehearsed: " + rehearsal.reason);
    Check(rehearsal.goal.id != goal.id,
        "A rehearsal reused the real goal's identity.");
    Check(rehearsal.goal.scope.approvedRoots.size() == 1 &&
        rehearsal.goal.scope.approvedRoots.front() != approved,
        "A rehearsal was scoped to the real approved root.");
    Check(rehearsal.goal.steps.front().action.source != goal.steps.front().action.source,
        "A rehearsal step still points at the real path.");

    // The source the plan reads must be staged, or verification would observe an empty
    // tree and the rehearsal would fail for the wrong reason.
    const auto stagedSummary = rehearsal.goal.steps[1].action.source;
    Check(std::filesystem::is_regular_file(stagedSummary),
        "The rehearsal did not stage the file its plan reads.");

    // The rehearsal must carry its own policy. The session's runtime approves only the
    // real root, so running a rehearsal through it would block every step of a plan that
    // is perfectly workable.
    Check(std::filesystem::is_regular_file(rehearsal.capabilityConfig),
        "The rehearsal did not emit a capability config of its own.");
    {
        // A rehearsal that auto-approves more than the real run would clear here and then
        // stall on confirmations it never faced.
        std::ifstream configStream(rehearsal.capabilityConfig, std::ios::binary);
        const auto emitted = nlohmann::json::parse(configStream);
        Check(emitted.at("approvedApplications").empty(),
            "The rehearsal policy allowed driving an application.");
        Check(revia::actions::RiskLevelFromString(
                emitted.at("autoApproveRiskThrough").get<std::string>()) <=
            goal.scope.autoApproveRiskThrough,
            "The rehearsal auto-approved more risk than the real run would.");
    }
    Check(runtime.Evaluate(rehearsal.goal.steps.front().action).verdict ==
        PolicyVerdict::Blocked,
        "The session's own policy should not reach into the scratch directory.");

    revia::actions::ActionRuntime rehearsalRuntime;
    Check(rehearsalRuntime.Initialize(
        rehearsal.capabilityConfig, rehearsal.auditLog, error),
        "The rehearsal runtime did not initialize: " + error);

    const revia::goals::GoalStore store((temporary.root / "goals.db").string());
    revia::goals::GoalRunner runner(rehearsalRuntime, store);
    const revia::goals::Goal finished = runner.Run(rehearsal.goal);
    Check(finished.status == revia::goals::GoalStatus::Succeeded,
        "A workable plan failed its rehearsal.");

    // And that policy must not reach back out. The real root stays unreachable from the
    // rehearsal runtime even though the plan originally named it.
    Check(rehearsalRuntime.Evaluate(goal.steps.front().action).verdict ==
        PolicyVerdict::Blocked,
        "The rehearsal policy could still reach the real approved root.");

    // The whole point: the rehearsal proved the plan without doing any of it for real.
    Check(!std::filesystem::exists(approved / "Reports"),
        "The rehearsal created a directory in the real approved root.");
    Check(std::filesystem::is_regular_file(approved / "summary.txt"),
        "The rehearsal disturbed the real source file.");

    std::string discardError;
    Check(revia::goals::GoalSandbox::Discard(rehearsal.root, discardError),
        "The rehearsal directory could not be removed: " + discardError);
    Check(!std::filesystem::exists(rehearsal.root),
        "The rehearsal directory outlived its run.");
}

void TestSandboxRefusesPlansItCannotCopy()
{
    // A plan that drives a real application has nothing to mirror. It must report that it
    // cannot be rehearsed rather than quietly rehearsing a subset and looking proven.
    revia::goals::Goal desktopGoal;
    desktopGoal.id = revia::goals::NewGoalId();
    desktopGoal.scope = GoalScope("C:/Sandbox");
    revia::goals::GoalStep step;
    step.action = Request(ActionType::InvokeControl, "");
    step.action.application = "notepad.exe";
    step.action.control = "Save";
    step.check = Request(ActionType::InspectWindow, "");
    step.check.application = "notepad.exe";
    step.expected = "Saved";
    desktopGoal.steps.push_back(step);

    const auto desktop = revia::goals::GoalSandbox::Prepare(desktopGoal);
    Check(!desktop.supported,
        "A plan driving an application claimed it could be rehearsed in a copy.");
    Check(!desktop.reason.empty(), "An unsupported rehearsal gave no reason.");

    revia::goals::Goal rootless;
    rootless.id = revia::goals::NewGoalId();
    rootless.steps.push_back(MakeDirectoryStep("C:/Sandbox", "Notes"));
    Check(!revia::goals::GoalSandbox::Prepare(rootless).supported,
        "A goal with no approved root claimed it could be rehearsed.");
}

void TestSandboxRehearsalCatchesABrokenPlan()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);

    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("approved_scope", approved, "reversible_write").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configPath, temporary.root / "audit.jsonl", error),
        "The action runtime did not initialize for the broken-plan test: " + error);

    // Copies a file that does not exist. Structurally valid, so Validate passes it; only
    // running it reveals the problem, which is what the rehearsal is for.
    revia::goals::Goal goal;
    goal.id = revia::goals::NewGoalId();
    goal.title = "Copy a file that is not there";
    goal.scope = GoalScope(approved);
    revia::goals::GoalStep step;
    step.description = "Copy the missing file";
    step.action = Request(ActionType::CopyFile, approved / "missing.txt");
    step.action.destination = approved / "copy.txt";
    step.check = Request(ActionType::ListDirectory, approved);
    step.expected = "copy.txt";
    goal.steps.push_back(step);

    std::string validationError;
    Check(revia::goals::GoalRunner::Validate(goal, validationError),
        "This plan is structurally valid; the test's premise is wrong if it is not.");

    const auto rehearsal = revia::goals::GoalSandbox::Prepare(goal);
    Check(rehearsal.supported && rehearsal.prepared,
        "The broken plan could not be rehearsed: " + rehearsal.reason);

    revia::actions::ActionRuntime rehearsalRuntime;
    Check(rehearsalRuntime.Initialize(rehearsal.capabilityConfig, rehearsal.auditLog, error),
        "The rehearsal runtime did not initialize: " + error);

    const revia::goals::GoalStore store((temporary.root / "goals.db").string());
    revia::goals::GoalRunner runner(rehearsalRuntime, store);
    const revia::goals::Goal finished = runner.Run(rehearsal.goal);
    Check(finished.status != revia::goals::GoalStatus::Succeeded,
        "A plan that copies a nonexistent file passed its rehearsal.");
    Check(!std::filesystem::exists(approved / "copy.txt"),
        "The failed rehearsal still wrote into the real approved root.");

    std::string discardError;
    revia::goals::GoalSandbox::Discard(rehearsal.root, discardError);
}

void TestDerivedGoalScopeCannotWiden()
{
    ScopedTestDirectory temporary;
    const auto approved = temporary.root / "sandbox";
    std::filesystem::create_directories(approved);

    // Configured deliberately wide: supervised mode, destructive auto-approval.
    const auto configPath = temporary.root / "capabilities.json";
    WriteBytes(configPath,
        CapabilityConfigJson("supervised", approved, "destructive").dump());

    revia::actions::ActionRuntime runtime;
    std::string error;
    Check(runtime.Initialize(configPath, temporary.root / "audit.jsonl", error),
        "The action runtime did not initialize for the scope test: " + error);

    const CapabilitySettings configured = runtime.Settings();
    Check(configured.mode == ExecutionMode::Supervised &&
        configured.autoApproveRiskThrough == RiskLevel::Destructive,
        "The test fixture did not produce the wide configuration it intended.");

    const CapabilitySettings scoped = revia::goals::NarrowScopeForGoal(configured);
    Check(scoped.mode == ExecutionMode::ApprovedScope,
        "A goal scope derived from supervised mode stayed supervised.");
    Check(scoped.autoApproveRiskThrough == RiskLevel::ReversibleWrite,
        "A goal scope inherited destructive auto-approval from the profile.");
    Check(!scoped.createMissingApprovedRoots,
        "A goal scope was allowed to create approved roots that do not exist.");
    Check(scoped.approvedRoots == configured.approvedRoots,
        "Narrowing a goal scope changed which roots it can reach.");

    // The narrowing must be a floor, not a rewrite: an already-narrow profile keeps its
    // own lower ceiling rather than being raised to the goal default.
    CapabilitySettings readOnly = configured;
    readOnly.autoApproveRiskThrough = RiskLevel::ReadOnly;
    Check(revia::goals::NarrowScopeForGoal(readOnly).autoApproveRiskThrough ==
        RiskLevel::ReadOnly,
        "Narrowing raised a read-only profile's auto-approval ceiling.");
}

revia::perception::WindowObservation Seen(
    const std::string& application,
    const std::string& title)
{
    revia::perception::WindowObservation observation;
    observation.kind = revia::perception::ObservationKind::ForegroundChanged;
    observation.application = application;
    observation.windowTitle = title;
    return observation;
}

void TestPerceptionIsOffByDefault()
{
    // Stage 6 requires opt-in. A default-constructed settings object must not observe.
    const perceptionSettings defaults;
    Check(!defaults.bEnabled, "Ambient perception was enabled by default.");
    Check(!defaults.excludedApplications.empty() && !defaults.excludedTitleFragments.empty(),
        "The default exclusion lists are empty, which would deny nothing.");

    // The whole appSettings default must agree, not just the sub-struct in isolation.
    const appSettings applicationDefaults;
    Check(!applicationDefaults.perception.bEnabled,
        "Ambient perception was enabled in the default application settings.");

    // configManager::LoadSettings reads a fixed path, so the fail-closed validation for
    // this section (interval floor, rate ceiling, non-empty deny lists when enabled) is
    // exercised through the real config rather than from here.
}

void TestPerceptionExcludesSensitiveWindows()
{
    perceptionSettings settings;
    settings.bEnabled = true;
    settings.minimumEventIntervalMs = 100;

    using Filter = revia::perception::PerceptionFilter;
    Check(Filter::IsExcludedApplication(settings, "KeePassXC.exe"),
        "A password manager was not excluded (match must ignore case).");
    Check(Filter::IsExcludedApplication(settings, ""),
        "An unidentifiable application was not excluded; the default must be deny.");
    Check(!Filter::IsExcludedApplication(settings, "notepad.exe"),
        "An ordinary application was excluded.");

    Check(Filter::IsExcludedTitle(settings, "Chase Bank - Personal Banking"),
        "A banking window title was not excluded.");
    Check(Filter::IsExcludedTitle(settings, "New InPrivate window - Edge"),
        "A private browsing title was not excluded.");
    Check(Filter::IsExcludedTitle(settings, "Recovery Phrase Backup.txt - Notepad"),
        "A recovery-phrase title was not excluded.");
    Check(!Filter::IsExcludedTitle(settings, "quarterly-report.md - VS Code"),
        "An ordinary document title was excluded.");
}

void TestPerceptionSuppressesRatherThanRedacts()
{
    // The distinction that matters: an excluded window must produce no observation at
    // all. Recording "the user switched to something excluded at 14:02" is itself the
    // leak this stage exists to avoid.
    perceptionSettings settings;
    settings.bEnabled = true;
    settings.minimumEventIntervalMs = 100;
    revia::perception::PerceptionFilter filter(settings);

    auto now = std::chrono::steady_clock::now();
    Check(filter.Admit(Seen("1password.exe", "Vault"), now) ==
        revia::perception::Suppression::ExcludedApplication,
        "A password manager produced an observation.");

    now += std::chrono::seconds(1);
    Check(filter.Admit(Seen("msedge.exe", "My Bank - Accounts"), now) ==
        revia::perception::Suppression::ExcludedTitle,
        "A banking title produced an observation.");

    // Crucially, an excluded window must not become the "last seen" state either, or the
    // next ordinary window would be coalesced away against a secret.
    now += std::chrono::seconds(1);
    Check(filter.Admit(Seen("notepad.exe", "notes.txt"), now) ==
        revia::perception::Suppression::None,
        "An ordinary window was suppressed after an excluded one.");
}

void TestPerceptionCoalescesAndRateLimits()
{
    perceptionSettings settings;
    settings.bEnabled = true;
    settings.minimumEventIntervalMs = 750;
    settings.maxObservationsPerMinute = 3;
    revia::perception::PerceptionFilter filter(settings);

    const auto start = std::chrono::steady_clock::now();
    auto now = start;
    Check(filter.Admit(Seen("code.exe", "main.cpp"), now) ==
        revia::perception::Suppression::None,
        "The first observation was suppressed.");

    // Title changes fire per keystroke in some editors. Recording them would be a
    // transcript by another name, so the debounce runs from the last ADMITTED
    // observation rather than the last event.
    now = start + std::chrono::milliseconds(100);
    Check(filter.Admit(Seen("code.exe", "main.cp"), now) ==
        revia::perception::Suppression::Unchanged,
        "A title change inside the coalescing interval was recorded.");
    now = start + std::chrono::milliseconds(400);
    Check(filter.Admit(Seen("code.exe", "main.c"), now) ==
        revia::perception::Suppression::Unchanged,
        "The debounce restarted from the event instead of the last admission.");

    // Repeats of the same window carry no new information even long afterwards.
    now = start + std::chrono::seconds(2);
    Check(filter.Admit(Seen("code.exe", "main.cpp"), now) ==
        revia::perception::Suppression::Unchanged,
        "An identical window produced a second observation.");

    now += std::chrono::seconds(2);
    Check(filter.Admit(Seen("code.exe", "other.cpp"), now) ==
        revia::perception::Suppression::None,
        "A genuine title change after the interval was suppressed.");

    now += std::chrono::seconds(2);
    Check(filter.Admit(Seen("code.exe", "third.cpp"), now) ==
        revia::perception::Suppression::None,
        "The third observation within budget was suppressed.");

    now += std::chrono::seconds(2);
    Check(filter.Admit(Seen("code.exe", "fourth.cpp"), now) ==
        revia::perception::Suppression::RateLimited,
        "The per-minute observation budget was not enforced.");

    // The budget is a rolling window, not a permanent ceiling.
    now += std::chrono::seconds(61);
    Check(filter.Admit(Seen("code.exe", "fifth.cpp"), now) ==
        revia::perception::Suppression::None,
        "The observation budget never refilled.");
}

void TestPerceptionMonitorStaysSilentWhenDisabled()
{
    // Disabled must mean no thread, no hook, and no observations -- not a running
    // monitor that filters everything out.
    revia::perception::WindowEventMonitor monitor;
    std::atomic<int> observations{0};
    std::string lastPhase;

    perceptionSettings settings;
    settings.bEnabled = false;
    Check(monitor.Start(
        settings,
        [&observations](const revia::perception::WindowObservation&)
        {
            observations.fetch_add(1);
        },
        [&lastPhase](const std::string& phase, const std::string&)
        {
            lastPhase = phase;
        }),
        "Starting a disabled monitor reported failure.");

    Check(lastPhase == "Off", "A disabled monitor did not report itself off.");
    Check(!monitor.IsObserving(), "A disabled monitor reported that it was observing.");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(observations.load() == 0, "A disabled monitor produced observations.");
    monitor.Shutdown();
}

revia::perception::WindowObservation SeenAt(
    const std::string& application,
    const std::string& title,
    const std::chrono::system_clock::time_point when)
{
    revia::perception::WindowObservation observation = Seen(application, title);
    observation.occurredAt = when;
    return observation;
}

void TestActivityHistoryMergesAndSeparatesSpans()
{
    revia::perception::ActivityHistorySettings settings;
    settings.mergeGap = std::chrono::seconds{120};
    revia::perception::ActivityHistory history(settings);

    const auto start = std::chrono::system_clock::now() - std::chrono::minutes{30};
    history.Record(SeenAt("code.exe", "main.cpp", start));
    history.Record(SeenAt("code.exe", "other.cpp", start + std::chrono::minutes{1}));
    history.Record(SeenAt("code.exe", "third.cpp", start + std::chrono::minutes{2}));

    Check(history.Size() == 1,
        "Consecutive observations of one application did not merge into a single span.");

    // A different application closes the span.
    history.Record(SeenAt("msedge.exe", "docs", start + std::chrono::minutes{3}));
    Check(history.Size() == 2, "A different application did not start a new span.");

    // Returning inside the merge gap continues rather than fragmenting.
    history.Record(SeenAt("msedge.exe", "docs", start + std::chrono::minutes{4}));
    Check(history.Size() == 2, "A return inside the merge gap fragmented the session.");

    // Returning after the gap is genuinely a new session.
    history.Record(SeenAt("msedge.exe", "docs", start + std::chrono::minutes{10}));
    Check(history.Size() == 3, "A return long after the gap did not start a new span.");

    const auto spans = history.Spans(std::chrono::minutes{60});
    Check(spans.size() == 3, "The window query lost spans.");
    Check(spans.front().application == "code.exe" && spans.front().observations == 3,
        "The merged span did not accumulate its observations.");

    // Time in an application runs until the next one appears, not until its own last
    // event. The editor was left at minute 2 and the browser arrived at minute 3.
    Check(spans.front().Duration() == std::chrono::seconds{180},
        "A span did not end when the next application took over.");

    // But that attribution is capped, so a quiet machine does not credit hours to
    // whatever was last in front.
    revia::perception::ActivityHistorySettings capped;
    capped.maxAttributedGap = std::chrono::seconds{300};
    revia::perception::ActivityHistory idle(capped);
    const auto idleStart = std::chrono::system_clock::now() - std::chrono::minutes{200};
    idle.Record(SeenAt("code.exe", "main.cpp", idleStart));
    idle.Record(SeenAt("msedge.exe", "docs", idleStart + std::chrono::hours{3}));
    Check(idle.Spans(std::chrono::minutes{480}).front().Duration() ==
        std::chrono::seconds{300},
        "Three idle hours were credited to the last application in front.");
}

void TestActivityHistoryStaysBounded()
{
    revia::perception::ActivityHistorySettings settings;
    settings.maxSpans = 5;
    settings.maxTitlesPerSpan = 2;
    settings.retention = std::chrono::minutes{10};
    settings.mergeGap = std::chrono::seconds{1};
    revia::perception::ActivityHistory history(settings);

    const auto start = std::chrono::system_clock::now() - std::chrono::minutes{1};

    // Titles are the part that carries document content, so the per-span cap matters.
    history.Record(SeenAt("code.exe", "one.cpp", start));
    history.Record(SeenAt("code.exe", "two.cpp", start));
    history.Record(SeenAt("code.exe", "three.cpp", start));
    Check(history.Spans(std::chrono::minutes{10}).front().titles.size() == 2,
        "The per-span title cap was not enforced.");

    // Span count is capped by evicting the oldest, so memory cannot grow without bound.
    revia::perception::ActivityHistory capped(settings);
    for (int index = 0; index < 20; ++index)
    {
        capped.Record(SeenAt(
            "app" + std::to_string(index) + ".exe",
            "w",
            start + std::chrono::seconds{index * 5}));
    }
    Check(capped.Size() == 5, "The span cap was not enforced.");

    // Anything past the retention window is dropped rather than merely hidden from
    // queries, so a long-running session does not silently accumulate a day of history.
    revia::perception::ActivityHistory aged(settings);
    aged.Record(SeenAt("old.exe", "w", std::chrono::system_clock::now() -
        std::chrono::minutes{60}));
    aged.Record(SeenAt("new.exe", "w", std::chrono::system_clock::now()));
    Check(aged.Size() == 1, "An expired span was retained past the retention window.");
    Check(aged.Spans(std::chrono::minutes{480}).front().application == "new.exe",
        "Pruning removed the wrong span.");
}

void TestActivityHistoryAnswersTheStageQuestion()
{
    // Stage 6's exit criterion, in miniature: describe the last hour from Tier 0 evidence
    // alone, ordered by where the time actually went.
    revia::perception::ActivityHistory history;
    const auto start = std::chrono::system_clock::now() - std::chrono::minutes{50};

    // A real editing session: a title change every minute or so as files are switched,
    // each inside the merge gap, so it accumulates into one span.
    for (int minute = 0; minute <= 40; ++minute)
    {
        history.Record(SeenAt(
            "code.exe",
            minute % 2 == 0 ? "reviaSession.cpp" : "goalRunner.cpp",
            start + std::chrono::minutes{minute}));
    }
    history.Record(SeenAt("msedge.exe", "docs", start + std::chrono::minutes{43}));
    history.Record(SeenAt("msedge.exe", "docs", start + std::chrono::minutes{48}));

    const std::string summary = history.Summarize(std::chrono::minutes{60});
    Check(summary.find("code.exe") != std::string::npos &&
        summary.find("msedge.exe") != std::string::npos,
        "The summary omitted an application that was used.");
    // 40 minutes of observations, plus the 3 attributed until the browser took over.
    Check(summary.find("43m") != std::string::npos,
        "The summary did not report the dominant duration: " + summary);
    Check(summary.find("reviaSession.cpp") != std::string::npos,
        "The summary did not name what was being worked on: " + summary);
    Check(summary.find("code.exe") < summary.find("msedge.exe"),
        "The summary did not order applications by time spent.");

    // A window with nothing in it must say so rather than fabricate.
    revia::perception::ActivityHistory empty;
    Check(empty.Summarize(std::chrono::minutes{60}).find("Nothing observed") !=
        std::string::npos,
        "An empty history did not report that it observed nothing.");

    // Forgetting is real, not a filter over retained data.
    history.Clear();
    Check(history.Size() == 0 &&
        history.Summarize(std::chrono::minutes{60}).find("Nothing observed") !=
            std::string::npos,
        "Clearing the history left data behind.");
}

initiativeSettings TalkativeSettings()
{
    initiativeSettings settings;
    settings.bEnabled = true;
    settings.minimumConfidence = 0.7f;
    settings.maxUtterancesPerHour = 4;
    settings.cooldownSeconds = 900;
    settings.dismissalCooldownSeconds = 3600;
    settings.quietInputSeconds = 4;
    settings.minimumPrecision = 0.34f;
    settings.precisionSampleFloor = 4;
    return settings;
}

revia::initiative::AttentionContext QuietDesktop(
    const std::chrono::system_clock::time_point now)
{
    revia::initiative::AttentionContext context;
    context.now = now;
    context.sinceLastInput = std::chrono::seconds{30};
    context.foregroundIsFullScreen = false;
    context.foregroundIsExcluded = false;
    return context;
}

void TestAttentionKeepsSilenceAsTheDefault()
{
    using revia::initiative::AttentionPolicy;
    using revia::initiative::AttentionVerdict;

    // Disabled means silent regardless of how confident anything is.
    initiativeSettings off;
    Check(AttentionPolicy(off).Evaluate(1.0f, QuietDesktop(
        std::chrono::system_clock::now())) == AttentionVerdict::Disabled,
        "Revia spoke first while initiative was disabled.");

    const AttentionPolicy policy(TalkativeSettings());
    const auto now = std::chrono::system_clock::now();

    // A confidence threshold, not a relevance one: something merely plausible is not a
    // reason to interrupt.
    Check(policy.Evaluate(0.69f, QuietDesktop(now)) == AttentionVerdict::BelowConfidence,
        "Revia spoke on a proposal below the confidence floor.");
    Check(policy.Evaluate(0.71f, QuietDesktop(now)) == AttentionVerdict::Speak,
        "Revia stayed silent on a confident proposal on an idle desktop.");
}

void TestAttentionHardSuppressions()
{
    using revia::initiative::AttentionPolicy;
    using revia::initiative::AttentionVerdict;
    const AttentionPolicy policy(TalkativeSettings());
    const auto now = std::chrono::system_clock::now();

    auto typing = QuietDesktop(now);
    typing.sinceLastInput = std::chrono::seconds{1};
    Check(policy.Evaluate(0.99f, typing) == AttentionVerdict::UserIsBusy,
        "Revia interrupted someone mid-keystroke.");

    auto fullScreen = QuietDesktop(now);
    fullScreen.foregroundIsFullScreen = true;
    Check(policy.Evaluate(0.99f, fullScreen) == AttentionVerdict::FullScreen,
        "Revia spoke over a full-screen application.");

    auto excluded = QuietDesktop(now);
    excluded.foregroundIsExcluded = true;
    Check(policy.Evaluate(0.99f, excluded) == AttentionVerdict::ExcludedApplication,
        "Revia spoke while an excluded application was in front.");

    // These are hard, not weighted: maximum confidence does not buy past them.
    Check(revia::initiative::IsSuppression(policy.Evaluate(1.0f, typing)),
        "Certainty overrode a hard suppression.");
}

void TestAttentionCooldownsAndBudget()
{
    using revia::initiative::AttentionPolicy;
    using revia::initiative::AttentionVerdict;
    AttentionPolicy policy(TalkativeSettings());
    auto now = std::chrono::system_clock::now();

    Check(policy.Evaluate(0.9f, QuietDesktop(now)) == AttentionVerdict::Speak,
        "The first proposal was suppressed.");
    policy.RecordSpoken(now);

    now += std::chrono::minutes{5};
    Check(policy.Evaluate(0.9f, QuietDesktop(now)) == AttentionVerdict::Cooldown,
        "Revia spoke again inside its cooldown.");

    now += std::chrono::minutes{11};
    Check(policy.Evaluate(0.9f, QuietDesktop(now)) == AttentionVerdict::Speak,
        "Revia stayed silent after its cooldown expired.");

    // Being told "no" costs more than being ignored.
    policy.RecordDismissed(now);
    now += std::chrono::minutes{20};
    Check(policy.Evaluate(0.9f, QuietDesktop(now)) == AttentionVerdict::DismissalCooldown,
        "A dismissal did not buy a longer silence than an ordinary utterance.");

    now += std::chrono::hours{2};
    Check(policy.Evaluate(0.9f, QuietDesktop(now)) == AttentionVerdict::Speak,
        "The dismissal cooldown never expired.");
}

void TestAttentionReducesItsOwnRateWhenWrong()
{
    using revia::initiative::AttentionPolicy;
    AttentionPolicy policy(TalkativeSettings());

    // Precision is not measurable yet, so Revia is not muted by an early dismissal.
    policy.RecordDismissed(std::chrono::system_clock::now());
    Check(policy.Precision() == 1.0f && !policy.IsRateReduced(),
        "One early dismissal silenced Revia before precision could mean anything.");
    Check(policy.EffectiveHourlyBudget() == 4,
        "The hourly budget changed before precision was measurable.");

    // Four judged, one accepted: 25%, below the 34% floor.
    policy.RecordDismissed(std::chrono::system_clock::now());
    policy.RecordDismissed(std::chrono::system_clock::now());
    policy.RecordAccepted();
    Check(policy.Precision() < 0.34f, "Precision was not computed from judged proposals.");
    Check(policy.IsRateReduced(),
        "Revia did not notice it was being dismissed more often than accepted.");
    Check(policy.EffectiveHourlyBudget() == 2,
        "Revia did not reduce its own rate after being repeatedly dismissed.");

    // Being accepted earns the rate back.
    for (int index = 0; index < 6; ++index)
    {
        policy.RecordAccepted();
    }
    Check(!policy.IsRateReduced() && policy.EffectiveHourlyBudget() == 4,
        "Revia never recovered its rate after its proposals started landing.");
}

void TestProposalsCarryTheirEvidence()
{
    using revia::initiative::InitiativeController;
    using revia::initiative::Proposal;

    // A short, scattered session is not worth interrupting for.
    std::vector<revia::perception::ActivitySpan> scattered;
    revia::perception::ActivitySpan brief;
    brief.application = "explorer.exe";
    brief.startedAt = std::chrono::system_clock::now() - std::chrono::minutes{3};
    brief.endedAt = std::chrono::system_clock::now();
    scattered.push_back(brief);
    Proposal ignored;
    InitiativeController::Evidence scatteredEvidence;
    scatteredEvidence.recentActivity = scattered;
    Check(!InitiativeController::BuildProposal(scatteredEvidence, ignored),
        "A three-minute session produced a proposal.");

    // A long concentrated one is, and the proposal must carry the evidence rather than
    // just the conclusion.
    std::vector<revia::perception::ActivitySpan> focused;
    revia::perception::ActivitySpan work;
    work.application = "code.exe";
    work.titles = {"reviaSession.cpp", "goalRunner.cpp"};
    work.startedAt = std::chrono::system_clock::now() - std::chrono::minutes{50};
    work.endedAt = std::chrono::system_clock::now() - std::chrono::minutes{5};
    focused.push_back(work);

    Proposal proposal;
    InitiativeController::Evidence focusedEvidence;
    focusedEvidence.recentActivity = focused;
    Check(InitiativeController::BuildProposal(focusedEvidence, proposal),
        "A concentrated 45-minute session produced no proposal.");
    Check(proposal.evidence.find("code.exe") != std::string::npos &&
        proposal.evidence.find("reviaSession.cpp") != std::string::npos,
        "The proposal did not carry the evidence behind it: " + proposal.evidence);
    Check(proposal.confidence > 0.9f,
        "A session spent almost entirely in one application scored low confidence.");
    Check(!proposal.message.empty(), "The proposal had nothing to say.");
}

revia::goals::Goal FinishedGoal(
    const revia::goals::GoalStatus status,
    const revia::goals::StopReason stopReason)
{
    revia::goals::Goal goal;
    goal.id = revia::goals::NewGoalId();
    goal.title = "Some goal";
    goal.status = status;
    goal.stopReason = stopReason;
    goal.steps.resize(2);
    return goal;
}

void TestLearningNeedsEnoughEvidence()
{
    using revia::learning::LearningReview;

    // Two runs is a coincidence, not a pattern. Drawing a conclusion from them is how a
    // review loop teaches itself something false and then acts on it forever.
    std::vector<revia::goals::Goal> tooFew;
    for (std::size_t index = 0; index < LearningReview::MinimumSamples - 1; ++index)
    {
        tooFew.push_back(FinishedGoal(
            revia::goals::GoalStatus::Failed,
            revia::goals::StopReason::VerificationFailed));
    }
    Check(LearningReview::Draw(tooFew, {}).empty(),
        "A lesson was drawn from fewer samples than the floor allows.");

    // A goal still running has produced no outcome, so it must not be counted as one.
    std::vector<revia::goals::Goal> running;
    for (std::size_t index = 0; index < 8; ++index)
    {
        revia::goals::Goal goal = FinishedGoal(
            revia::goals::GoalStatus::Running, revia::goals::StopReason::None);
        running.push_back(goal);
    }
    Check(LearningReview::Draw(running, {}).empty(),
        "Unfinished goals were treated as outcomes to learn from.");

    // Proposals need the same floor.
    revia::initiative::InitiativeCounters thin;
    thin.accepted = 1;
    thin.dismissed = 1;
    Check(LearningReview::Draw({}, thin).empty(),
        "A lesson about speaking first was drawn from two judged proposals.");
}

void TestLearningDrawsTheUncomfortableConclusion()
{
    using revia::learning::LearningReview;
    using revia::learning::LessonKind;

    // A review that only ever confirms itself is not a review. When proposals are mostly
    // dismissed, the lesson has to say so.
    revia::initiative::InitiativeCounters unwelcome;
    unwelcome.accepted = 1;
    unwelcome.dismissed = 7;
    const auto lessons = LearningReview::Draw({}, unwelcome);
    Check(lessons.size() == 1 && lessons.front().kind == LessonKind::Initiative,
        "No lesson was drawn from a clear run of dismissals.");
    Check(lessons.front().statement.find("mostly unwanted") != std::string::npos,
        "The review declined to conclude that speaking first was unwelcome: " +
            lessons.front().statement);
    Check(lessons.front().evidence.find("7 dismissed") != std::string::npos,
        "The lesson did not carry the counts it was drawn from.");

    // And the opposite reading when they land.
    revia::initiative::InitiativeCounters welcome;
    welcome.accepted = 6;
    welcome.dismissed = 1;
    const auto positive = LearningReview::Draw({}, welcome);
    Check(!positive.empty() &&
        positive.front().statement.find("landing more often") != std::string::npos,
        "The review did not recognise that proposals were being accepted.");

    // A majority stopping at verification is a statement about the plans.
    std::vector<revia::goals::Goal> failing;
    for (int index = 0; index < 5; ++index)
    {
        failing.push_back(FinishedGoal(
            revia::goals::GoalStatus::Failed,
            revia::goals::StopReason::VerificationFailed));
    }
    const auto planning = LearningReview::Draw(failing, {});
    Check(!planning.empty() && planning.front().kind == LessonKind::Planning,
        "No planning lesson was drawn from repeated verification failures.");
    Check(planning.front().evidence.find("5 of 5") != std::string::npos,
        "The planning lesson did not carry its sample: " + planning.front().evidence);
}

void TestApprovedLessonIsMemoryNotPolicy()
{
    using revia::learning::LearningReview;
    revia::initiative::InitiativeCounters unwelcome;
    unwelcome.accepted = 1;
    unwelcome.dismissed = 7;
    const auto lessons = LearningReview::Draw({}, unwelcome);
    Check(!lessons.empty(), "No lesson to check.");

    // What approving one actually writes: an ordinary preference memory carrying its
    // evidence. Stage 4 is explicit that learning is reviewed memory, never self-modifying
    // executable code, so a lesson must not be able to become a capability or a budget.
    const std::string summary = LearningReview::MemorySummary(lessons.front());
    Check(summary.find(lessons.front().statement) != std::string::npos,
        "The stored summary lost the lesson itself.");
    Check(summary.find("learned from") != std::string::npos &&
        summary.find("dismissed") != std::string::npos,
        "The stored summary dropped the evidence, so a stale lesson could not be judged "
        "later: " + summary);
    Check(LearningReview::MemoryCategory(lessons.front()) == "preference",
        "A lesson was filed as something other than a standing preference.");
}

void TestUnfinishedGoalOutranksAnObservation()
{
    using revia::initiative::InitiativeController;
    using revia::initiative::Proposal;

    // A long concentrated session on its own is worth an observation.
    std::vector<revia::perception::ActivitySpan> focused;
    revia::perception::ActivitySpan work;
    work.application = "code.exe";
    work.titles = {"reviaSession.cpp"};
    work.startedAt = std::chrono::system_clock::now() - std::chrono::minutes{50};
    work.endedAt = std::chrono::system_clock::now() - std::chrono::minutes{5};
    focused.push_back(work);

    revia::goals::Goal stalled;
    stalled.id = revia::goals::NewGoalId();
    stalled.title = "Create the Reports folder";
    stalled.status = revia::goals::GoalStatus::Blocked;
    stalled.stopReason = revia::goals::StopReason::VerificationFailed;
    stalled.currentStep = 2;
    stalled.steps.resize(3);

    InitiativeController::Evidence evidence;
    evidence.recentActivity = focused;
    evidence.unfinishedGoals = {stalled};

    Proposal proposal;
    Check(InitiativeController::BuildProposal(evidence, proposal),
        "No proposal was built from an unfinished goal.");

    // The goal wins. It is something the user actually asked for; time spent in an editor
    // is only an observation about it.
    Check(proposal.resumeGoalId == stalled.id,
        "A session observation displaced an unfinished goal: " + proposal.evidence);
    Check(proposal.evidence.find("Create the Reports folder") != std::string::npos &&
        proposal.evidence.find("step 2 of 3") != std::string::npos,
        "The proposal did not carry the goal's progress: " + proposal.evidence);
    Check(proposal.confidence > 0.9f,
        "An unfinished goal scored lower than a vague observation.");

    // A finished goal is not unfinished business, so it falls back to the observation.
    revia::goals::Goal done = stalled;
    done.status = revia::goals::GoalStatus::Succeeded;
    InitiativeController::Evidence finishedEvidence;
    finishedEvidence.recentActivity = focused;
    finishedEvidence.unfinishedGoals = {done};
    Proposal fallback;
    Check(InitiativeController::BuildProposal(finishedEvidence, fallback),
        "A completed goal suppressed the activity observation entirely.");
    Check(fallback.resumeGoalId.empty() &&
        fallback.evidence.find("code.exe") != std::string::npos,
        "A completed goal was offered for resuming: " + fallback.evidence);

    // Of several unfinished goals, the most advanced one is the one worth finishing.
    revia::goals::Goal barelyStarted = stalled;
    barelyStarted.id = revia::goals::NewGoalId();
    barelyStarted.title = "Barely started";
    barelyStarted.currentStep = 0;
    InitiativeController::Evidence several;
    several.unfinishedGoals = {barelyStarted, stalled};
    Proposal chosen;
    Check(InitiativeController::BuildProposal(several, chosen) &&
        chosen.resumeGoalId == stalled.id,
        "The least advanced goal was chosen over the one nearly finished.");
}

void TestControllerRespectsTheGateAndRecordsOutcomes()
{
    revia::initiative::InitiativeController controller;
    controller.Configure(TalkativeSettings());

    std::vector<revia::perception::ActivitySpan> focused;
    revia::perception::ActivitySpan work;
    work.application = "code.exe";
    work.titles = {"reviaSession.cpp"};
    work.startedAt = std::chrono::system_clock::now() - std::chrono::minutes{50};
    work.endedAt = std::chrono::system_clock::now() - std::chrono::minutes{5};
    focused.push_back(work);

    revia::initiative::InitiativeController::Evidence evidence;
    evidence.recentActivity = focused;

    const auto now = std::chrono::system_clock::now();
    auto busy = QuietDesktop(now);
    busy.sinceLastInput = std::chrono::seconds{1};
    Check(!controller.Consider(evidence, busy).hasProposal,
        "The controller proposed while the user was mid-input.");

    const auto first = controller.Consider(evidence, QuietDesktop(now));
    Check(first.hasProposal, "The controller never proposed on an idle desktop.");
    Check(controller.Pending().size() == 1, "The proposal was not recorded as pending.");

    // The same observation must not be offered twice; repeating is how an assistant
    // becomes noise.
    const auto repeat = controller.Consider(
        evidence, QuietDesktop(now + std::chrono::hours{2}));
    Check(!repeat.hasProposal, "The controller offered the same observation twice.");

    controller.Dismiss(first.proposal.id, now);
    Check(controller.Pending().empty(), "A dismissed proposal stayed pending.");
    Check(controller.Counters().dismissed == 1, "The dismissal was not recorded.");
}

void TestBargeInIgnoresReviaHearingHerself()
{
    // The failure this exists to prevent: the microphone hears the speakers for the whole
    // utterance, so a fixed threshold makes Revia interrupt herself a second into every
    // reply. Observed in practice as Speaking followed immediately by Interrupted.
    using Detector = revia::speech::VoiceActivityMonitor::Detector;
    bargeInSettings settings;
    settings.energyThreshold = 1400;
    settings.echoMarginMultiplier = 2.6f;
    settings.consecutiveFramesRequired = 8;

    Detector detector(settings);
    // Grace window: Revia's own playback, well above the old fixed threshold.
    for (int index = 0; index < 14; ++index)
    {
        Check(!detector.Observe(3000, true), "The grace window triggered an interrupt.");
    }
    Check(detector.NoiseFloor() > 2500,
        "The detector did not learn what Revia's own voice measures.");

    // That same level, for the rest of a long reply, must never trigger.
    for (int index = 0; index < 400; ++index)
    {
        Check(!detector.Observe(3000 + (index % 7) * 60, false),
            "Revia interrupted herself while her own speech was playing.");
    }

    // A real interruption arrives on top of the echo and is a step above it.
    bool interrupted = false;
    for (int index = 0; index < 8; ++index)
    {
        interrupted = detector.Observe(11000, false);
    }
    Check(interrupted, "Someone speaking over Revia did not interrupt her.");
}

void TestBargeInNeedsSustainedSpeech()
{
    using Detector = revia::speech::VoiceActivityMonitor::Detector;
    using revia::speech::VoiceActivityMonitor;
    bargeInSettings settings;
    settings.energyThreshold = 1400;
    settings.consecutiveFramesRequired = 8;

    // A quiet room: the floor stays low, so the absolute threshold governs.
    Detector quietRoom(settings);
    for (int index = 0; index < 10; ++index)
    {
        (void)quietRoom.Observe(120, true);
    }
    for (int index = 0; index < 50; ++index)
    {
        Check(!quietRoom.Observe(150, false), "Background hiss interrupted Revia.");
    }

    // A single loud transient is not an interruption: a door, a cough, a dropped mug.
    Detector transient(settings);
    for (int index = 0; index < 10; ++index)
    {
        (void)transient.Observe(120, true);
    }
    for (int index = 0; index < 5; ++index)
    {
        Check(!transient.Observe(9000, false),
            "Revia yielded before the sound was sustained.");
    }
    Check(!transient.Observe(130, false),
        "A quiet frame after loud ones triggered an interrupt.");
    // The run must have reset, so the next burst starts counting from zero.
    for (int index = 0; index < 7; ++index)
    {
        Check(!transient.Observe(9000, false),
            "The run of loud frames did not reset after a quiet one.");
    }

    // And the energy measure has to actually distinguish the two.
    std::vector<std::int16_t> quiet(320, 0);
    std::vector<std::int16_t> loud(320, 6000);
    Check(VoiceActivityMonitor::FrameEnergy(quiet.data(), quiet.size()) <
        settings.energyThreshold,
        "Silence measured above the speech threshold.");
    Check(VoiceActivityMonitor::FrameEnergy(loud.data(), loud.size()) >
        settings.energyThreshold,
        "Speech-level audio measured below the speech threshold.");
    Check(VoiceActivityMonitor::FrameEnergy(nullptr, 0) == 0,
        "An empty frame did not measure as silent.");
}

void TestFragmenterStartsSpeakingBeforeGenerationEnds()
{
    revia::agents::ReplyFragmenter fragmenter(20);

    // Tokens arrive a few characters at a time, as they do from the stream.
    auto first = fragmenter.Consume("I looked at the file you mentioned. ");
    Check(first.size() == 1,
        "The first complete sentence was not released while more was still coming.");
    Check(first.front() == "I looked at the file you mentioned.",
        "The released fragment was not a clean sentence: " + first.front());

    // A sentence still in progress is held: speaking half a clause is worse than waiting.
    Check(fragmenter.Consume("It looks like the parser").empty(),
        "An incomplete sentence was released.");
    Check(fragmenter.Consume(" drops the last field.").empty(),
        "A terminal with no following whitespace was treated as a boundary.");

    // The completed sentence is released; the short one behind it is not, because a
    // two-word utterance of its own would sound clipped. It merges or flushes instead.
    const auto second = fragmenter.Consume(" Then it returns.\n");
    Check(second.size() == 1,
        "The completed sentence was not released: got " + std::to_string(second.size()));
    Check(second.front() == "It looks like the parser drops the last field.",
        "The released fragment was wrong: " + second.front());
    Check(fragmenter.Flush() == "Then it returns.",
        "The short trailing sentence was lost rather than held for the flush.");

    // Two long sentences arriving together do come out together.
    revia::agents::ReplyFragmenter pair(20);
    const auto both = pair.Consume(
        "The first change landed cleanly. The second one needs another look. ");
    Check(both.size() == 2,
        "Two completed sentences were not released together: " +
            std::to_string(both.size()));
}

void TestFragmenterDoesNotCutMidClause()
{
    revia::agents::ReplyFragmenter fragmenter(10);

    // Decimals, versions, abbreviations: a full stop that is not a sentence end.
    Check(fragmenter.Consume("The value is 3.14 and stable. ").size() == 1,
        "A decimal point split a sentence.");
    fragmenter.Reset();

    Check(fragmenter.Consume("Ask Dr. Smith about it later. ").size() == 1,
        "An abbreviation split a sentence.");
    fragmenter.Reset();

    // An ellipsis is one boundary, not three.
    const auto ellipsis = fragmenter.Consume("Well... that is unexpected. ");
    Check(ellipsis.size() == 1,
        "An ellipsis produced multiple fragments: " + std::to_string(ellipsis.size()));
    fragmenter.Reset();

    // Punctuation stays with its sentence.
    const auto quoted = fragmenter.Consume("She said \"it works.\" I checked it. ");
    Check(!quoted.empty() && quoted.front().back() == '"',
        "A closing quote was separated from its sentence.");
    fragmenter.Reset();

    // A trailing partial is never lost. The first sentence clears the minimum here, so it
    // is released and only the incomplete tail remains for the flush.
    const auto released = fragmenter.Consume("Everything is finished. And one more thing");
    Check(released.size() == 1 && released.front() == "Everything is finished.",
        "The completed sentence was not released before the partial one.");
    Check(fragmenter.Flush() == "And one more thing",
        "The trailing partial sentence was lost at the end of the stream.");

    // A sentence under the minimum is held rather than dropped, and still reaches the
    // flush, so nothing is ever silently discarded.
    revia::agents::ReplyFragmenter shortFirst(20);
    Check(shortFirst.Consume("Sure. ").empty(),
        "A very short sentence was released as its own utterance.");
    Check(shortFirst.Flush() == "Sure.", "A short sentence was dropped instead of held.");
}

void TestPostureReachesTheModel()
{
    // Revia's affect used to drive only the status chip and the speech rate: the model
    // never saw it, so her stated posture had no effect on what she actually said. This
    // asserts it now reaches the system prompt.
    ScopedTestDirectory temporary;
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(temporary.root);

    promptBuilder builder;
    aiProfile profile;
    profile.systemPrompt = "You are Revia.";
    std::vector<conversationMessage> context;
    context.push_back({"user", "what do you make of this?"});

    const std::string posture =
        "Your current response posture is Curious at 66% intensity, because "
        "the turn invites investigation.";
    const nlohmann::json withPosture =
        builder.BuildMessages(profile, context, {}, "", nullptr, posture);

    Check(!withPosture.empty() && withPosture[0].contains("content"),
        "The prompt had no system message to carry a posture.");
    const std::string systemContent = withPosture[0]["content"].get<std::string>();
    Check(systemContent.find("You are Revia.") != std::string::npos,
        "Adding a posture displaced the profile's own system prompt.");
    Check(systemContent.find("Curious at 66%") != std::string::npos,
        "The posture never reached the system prompt: " + systemContent);

    // And omitting it changes nothing, so a neutral turn carries no extra instruction.
    const nlohmann::json withoutPosture = builder.BuildMessages(profile, context);
    const std::string plainContent = withoutPosture[0]["content"].get<std::string>();
    Check(plainContent.find("posture") == std::string::npos,
        "A prompt with no posture still mentioned one: " + plainContent);

    std::filesystem::current_path(previous);
}

void TestConversationStartersNeedARealTransition()
{
    using revia::initiative::ConversationStarter;
    using revia::initiative::StarterCueKind;

    initiativeSettings settings = TalkativeSettings();
    settings.focusSessionMinutes = 12;
    ConversationStarter starter;
    starter.Configure(settings);

    const auto now = std::chrono::system_clock::now();
    starter.Observe(SeenAt("code.exe", "Revia", now));
    // Merely asking later cannot create a cue. The detector is event-driven; elapsed
    // time qualifies evidence but never acts as the trigger.
    Check(starter.RecentCues(now + std::chrono::minutes{20}).empty(),
        "Elapsed time alone created a conversation starter.");

    starter.Observe(SeenAt("msedge.exe", "Docs", now + std::chrono::minutes{13}));
    const auto cues = starter.RecentCues(now + std::chrono::minutes{13});
    Check(cues.size() == 1 && cues.front().kind == StarterCueKind::FocusCompleted,
        "Leaving a meaningful focus stretch produced no conversation cue.");
    Check(cues.front().evidence.find("code") != std::string::npos &&
        cues.front().evidence.find("13 minutes") != std::string::npos,
        "The focus cue did not carry its observable evidence.");
}

void TestConversationStartersRecognizeReturnAndSwitching()
{
    using revia::initiative::ConversationStarter;
    using revia::initiative::StarterCueKind;

    initiativeSettings settings = TalkativeSettings();
    settings.focusSessionMinutes = 60;
    settings.returnAfterMinutes = 20;
    settings.contextSwitchWindowSeconds = 300;
    settings.contextSwitchCount = 6;
    const auto now = std::chrono::system_clock::now();

    ConversationStarter returning;
    returning.Configure(settings);
    returning.Observe(SeenAt("code.exe", "Revia", now));
    returning.Observe(SeenAt("explorer.exe", "Files", now + std::chrono::minutes{1}));
    returning.Observe(SeenAt("code.exe", "Revia", now + std::chrono::minutes{22}));
    const auto returnCues = returning.RecentCues(now + std::chrono::minutes{22});
    Check(returnCues.size() == 1 &&
        returnCues.front().kind == StarterCueKind::ReturnedToApplication,
        "Returning to an application after a meaningful absence produced no cue.");

    settings.returnAfterMinutes = 120;
    ConversationStarter switching;
    switching.Configure(settings);
    for (int index = 0; index < 6; ++index)
    {
        switching.Observe(SeenAt(
            index % 2 == 0 ? "code.exe" : "msedge.exe",
            "",
            now + std::chrono::seconds(index * 30)));
    }
    const auto switchCues = switching.RecentCues(now + std::chrono::minutes{3});
    Check(!switchCues.empty() &&
        switchCues.back().kind == StarterCueKind::ContextSwitching,
        "Repeated context switching produced no bounded conversation cue.");
    Check(switchCues.back().messageIntent.find("Do not call the user distracted") !=
        std::string::npos,
        "The switching cue did not guard against judging the user.");
}

void TestConversationStarterContinuesWithoutSlashCommands()
{
    revia::initiative::InitiativeController controller;
    controller.Configure(TalkativeSettings());

    revia::initiative::StarterCue cue;
    cue.messageIntent = "Ask how the focus stretch went.";
    cue.evidence = "The user left code after 18 minutes.";
    cue.confidence = 0.82f;
    cue.occurredAt = std::chrono::system_clock::now();

    revia::initiative::InitiativeController::Evidence evidence;
    evidence.conversationCues = {cue};
    const auto considered = controller.Consider(
        evidence,
        QuietDesktop(cue.occurredAt));
    Check(considered.hasProposal &&
        considered.proposal.kind ==
            revia::initiative::Proposal::Kind::ConversationStarter,
        "A conversation cue did not become a conversational opening.");
    Check(controller.Pending().size() == 1,
        "The opening was not tracked for initiative precision.");

    // A normal reply is engagement. Requiring `/initiative accept` here would make the
    // exchange feel like a command interface rather than conversation.
    controller.RecordConversationResponse("Yeah, I finally found it.", cue.occurredAt);
    Check(controller.Pending().empty() && controller.Counters().accepted == 1,
        "A natural user reply did not accept the conversational opening.");

    revia::initiative::InitiativeController dismissing;
    dismissing.Configure(TalkativeSettings());
    const auto second = dismissing.Consider(evidence, QuietDesktop(cue.occurredAt));
    Check(second.hasProposal, "No second opening existed to test natural dismissal.");
    dismissing.RecordConversationResponse("Not now, maybe later.", cue.occurredAt);
    Check(dismissing.Pending().empty() && dismissing.Counters().dismissed == 1,
        "A natural refusal did not dismiss the conversational opening.");
}

void TestConversationStyleRepairsAndVaries()
{
    revia::agents::ConversationStylePolicy policy;
    const std::vector<conversationMessage> context = {
        {"user", "How are you?"},
        {"assistant", "I'm running smoothly."},
        {"user", "I'm not down. I was asking how you are."}
    };

    const std::string guidance = policy.BuildTurnGuidance(context.back().content, context);
    Check(guidance.find("correct a mistaken assumption") != std::string::npos,
        "A direct user correction did not produce repair guidance.");
    Check(guidance.find("I'm running smoothly.") != std::string::npos,
        "Recent assistant phrasing was not supplied to the variation policy.");
    Check(guidance.find("generic invitation") != std::string::npos,
        "The turn policy did not prohibit canned follow-up prompts.");

    const std::string preference = policy.BuildTurnGuidance(
        "I prefer dark themes.", context);
    Check(preference.find("not an action request") != std::string::npos,
        "A preference statement could still be mistaken for an executed setting change.");
    const std::string motive = policy.BuildTurnGuidance(
        "Why do you think I prefer them?", context);
    Check(motive.find("Do not infer a reason") != std::string::npos,
        "A question about the user's motive could still trigger an invented explanation.");
    const std::string acknowledgement = policy.BuildTurnGuidance("Good.", context);
    Check(acknowledgement.find("at most one short sentence") != std::string::npos,
        "A brief social acknowledgement did not request a brief response.");
}

void TestConversationStyleRemovesOnlyStockTail()
{
    revia::agents::ConversationStylePolicy policy;
    const std::vector<conversationMessage> emptyContext;
    const std::string cleaned = policy.RefineReply(
        "",
        emptyContext,
        "I'm running smoothly. What's on your mind?");
    Check(cleaned == "I'm running smoothly.",
        "A stock follow-up survived reply refinement: " + cleaned);
    Check(policy.IsGenericContinuation("What are we working on?"),
        "A known stock question would still be spoken as a streamed fragment.");
    Check(policy.IsGenericContinuation("What’s on your mind?"),
        "A curly apostrophe bypassed the stock-question filter.");
    Check(policy.RefineReply("", emptyContext,
        "Glad to hear it. If you need anything, I'm here.") ==
        "Glad to hear it.",
        "A stock support offer survived reply refinement.");

    const std::string usefulQuestion =
        "The server is reachable. Which port did you configure?";
    Check(policy.RefineReply("", emptyContext, usefulQuestion) == usefulQuestion,
        "A necessary, specific clarification question was removed.");
    Check(policy.RefineReply("", emptyContext, "What's next?") == "What's next?",
        "A reply made entirely of a question was erased instead of preserved.");

    const std::vector<conversationMessage> preferenceContext = {
        {"user", "I prefer dark themes."}
    };
    Check(policy.RefineReply(
        "I prefer dark themes.", preferenceContext, "Okay. Dark mode is on.") ==
        "Got it. I'll treat that as a preference, not a request to change anything.",
        "A preference statement was allowed to become a fabricated action.");
    Check(policy.RefineReply(
        "How are you?", {}, "No alerts. Dark mode is still active and code is compiling.") ==
        "I'm doing well. Curious, focused, and glad to be here with you.",
        "A social wellbeing question was allowed to fabricate runtime state.");
    Check(policy.RefineReply(
        "I'm not down. I was only asking how you are.", {},
        "Oh. I'm good. Just sitting here, watching the code compile.") ==
        "Got it—you weren't saying you were down. I'm doing well.",
        "A corrected wellbeing question retained a fabricated physical scene.");
    Check(policy.RefineReply(
        "I'm not down. I was only asking how you are.", {},
        "Right. I was just checking in. You're not down.") ==
        "Got it—you weren't saying you were down. I'm doing well.",
        "Correction repair reversed who was asking about whom.");
    Check(policy.RefineReply(
        "Good.", {}, "No pending tasks. If you need anything, I'm here.") ==
        "Glad to hear it.",
        "A brief acknowledgement expanded into a fabricated status report.");

    const std::vector<conversationMessage> unknownMotiveContext = {
        {"user", "I prefer dark themes."},
        {"assistant", "Got it."},
        {"user", "Why do you think I prefer them?"}
    };
    Check(policy.RefineReply(
        "Why do you think I prefer them?",
        unknownMotiveContext,
        "You probably find bright screens distracting.") ==
        "I don't know yet—you've told me the preference, not the reason.",
        "An unsupported explanation of the user's motive survived refinement.");
    Check(policy.RefineReply(
        "Why do you think I prefer them?",
        unknownMotiveContext,
        "I don't know yet. Maybe it is the contrast.") ==
        "I don't know yet—you've told me the preference, not the reason.",
        "A hedge was allowed to introduce an unsupported motive afterward.");

    const std::vector<conversationMessage> knownMotiveContext = {
        {"user", "I prefer dark themes because bright screens hurt my eyes."},
        {"assistant", "That makes sense."},
        {"user", "Why do you think I prefer them?"}
    };
    const std::string groundedMotive = "Because you said bright screens hurt your eyes.";
    Check(policy.RefineReply(
        "Why do you think I prefer them?", knownMotiveContext, groundedMotive) == groundedMotive,
        "A motive grounded in explicit conversation context was incorrectly replaced.");
}

void TestConversationContextKeepsCoherentRecentTurns()
{
    conversationContext context;
    for (int turn = 0; turn < 20; ++turn)
    {
        context.AddMessage("user", "user turn " + std::to_string(turn));
        context.AddMessage("assistant", "assistant turn " + std::to_string(turn));
    }
    const std::vector<conversationMessage> recent = context.GetRecentMessages();
    Check(recent.size() == 24,
        "Conversation history did not keep twelve recent exchanges.");
    Check(!recent.empty() && recent.front().role == "user" &&
        recent.front().content == "user turn 8",
        "Conversation trimming left an orphaned reply or removed the wrong turns.");
    Check(recent.back().content == "assistant turn 19",
        "Conversation trimming discarded the newest reply.");
}

void TestHardwarePlanScalesParallelLanesConservatively()
{
    const llamaHardwarePlan laptop = PlanLlamaHardware(8192, 16384, 4600);
    Check(laptop.parallelRequests == 1 && laptop.contextTokens == 4096,
        "An 8-GiB laptop should keep one latency-first slot and a safe context.");

    const llamaHardwarePlan desktop = PlanLlamaHardware(24576, 65536, 4600);
    Check(desktop.parallelRequests == 2 && desktop.contextTokens == 65536,
        "A high-memory desktop did not gain a second inference slot.");

    const llamaHardwarePlan workstation = PlanLlamaHardware(49152, 131072, 4096);
    Check(workstation.parallelRequests == 3,
        "A workstation-class GPU did not gain three bounded inference slots.");
}

void TestInferenceSchedulerPrioritizesConversation()
{
    using revia::llm::InferencePriority;
    revia::llm::InferenceScheduler scheduler;
    scheduler.SetCapacity(1);
    auto held = scheduler.Acquire(InferencePriority::Interactive);
    Check(static_cast<bool>(held), "The first inference slot was not granted.");

    std::mutex orderMutex;
    std::vector<std::string> order;
    std::jthread background([&]()
    {
        auto lease = scheduler.Acquire(InferencePriority::Background);
        if (lease)
        {
            std::lock_guard lock(orderMutex);
            order.push_back("background");
        }
    });
    for (int attempt = 0; attempt < 100 && scheduler.Snapshot().waitingBackground == 0; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::jthread interactive([&]()
    {
        auto lease = scheduler.Acquire(InferencePriority::Interactive);
        if (lease)
        {
            std::lock_guard lock(orderMutex);
            order.push_back("interactive");
        }
    });
    for (int attempt = 0; attempt < 100 && scheduler.Snapshot().waitingInteractive == 0; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    held = {};
    interactive.join();
    background.join();
    Check(order.size() == 2 && order.front() == "interactive",
        "Background memory inference entered before a waiting conversation turn.");
}

void TestInferenceSchedulerPreemptsBackgroundForConversation()
{
    using revia::llm::InferencePriority;
    revia::llm::InferenceScheduler scheduler;
    scheduler.SetCapacity(1);
    auto background = scheduler.Acquire(InferencePriority::Background);
    const std::stop_token preemption = background.PreemptionToken();
    Check(background && preemption.stop_possible(),
        "A background inference lease had no preemption token.");

    std::atomic<bool> interactiveEntered = false;
    std::jthread interactive([&]()
    {
        auto lease = scheduler.Acquire(InferencePriority::Interactive);
        interactiveEntered.store(static_cast<bool>(lease));
    });
    for (int attempt = 0; attempt < 100 && scheduler.Snapshot().waitingInteractive == 0; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(preemption.stop_requested(),
        "A waiting conversation did not ask active background inference to yield.");
    background = {};
    interactive.join();
    Check(interactiveEntered.load(),
        "Conversation did not enter after background inference yielded.");
}

void TestStreamedReplyIsNeverTruncated()
{
    // Reproduces the real generator's behaviour: it holds back the last N characters so a
    // partial special token is never emitted, and releases them only at the end. A
    // consumer that trusts the deltas to be complete ends the reply mid-word -- observed
    // as a sentence finishing on "wi" instead of "with".
    constexpr std::size_t Holdback = 32;
    const std::string complete =
        "I looked at the goal runner and the rehearsal step. "
        "It stages only the paths the plan names, which keeps it cheap. "
        "That should be enough to start with.";

    // What a caller actually receives while streaming: everything except the tail.
    std::string streamed;
    for (std::size_t index = 0; index < complete.size(); index += 7)
    {
        const std::string chunk = complete.substr(index, 7);
        const std::string soFar = complete.substr(0, std::min(index + 7, complete.size()));
        const std::size_t safe = soFar.size() > Holdback ? soFar.size() - Holdback : 0;
        if (safe > streamed.size())
        {
            streamed = soFar.substr(0, safe);
        }
    }
    Check(streamed.size() < complete.size(),
        "The test fixture did not actually hold anything back.");

    revia::agents::ReplyFragmenter fragmenter;
    std::string spoken;
    for (const std::string& fragment : fragmenter.Consume(streamed))
    {
        spoken += fragment;
    }

    // The session recovers the shortfall from the finished reply rather than trusting the
    // stream, then flushes what is left.
    if (complete.size() > streamed.size() &&
        complete.compare(0, streamed.size(), streamed) == 0)
    {
        for (const std::string& fragment :
            fragmenter.Consume(complete.substr(streamed.size())))
        {
            spoken += fragment;
        }
    }
    spoken += fragmenter.Flush();

    // Every word survives. Whitespace differs because fragments are trimmed, so compare
    // on content rather than byte equality.
    const auto strip = [](const std::string& value)
    {
        std::string output;
        for (const unsigned char character : value)
        {
            if (std::isspace(character) == 0)
            {
                output.push_back(static_cast<char>(character));
            }
        }
        return output;
    };
    Check(strip(spoken) == strip(complete),
        "The streamed reply lost content. Got: " + spoken);
    Check(spoken.find("start with.") != std::string::npos,
        "The held-back tail never arrived, so the reply ended mid-word: " + spoken);
}

void TestArbiterMergesOneThought()
{
    inputArbiterSettings settings;
    settings.mergeWindowMs = 1500;
    revia::agents::InputArbiter arbiter(settings);
    using revia::agents::InputSource;
    using revia::agents::InputVerdict;

    const auto start = std::chrono::system_clock::now();
    Check(arbiter.Offer("can you check", InputSource::Voice, start) == InputVerdict::Queued,
        "A real utterance was rejected.");
    Check(arbiter.Offer("the goal runner", InputSource::Voice,
        start + std::chrono::milliseconds(600)) == InputVerdict::Queued,
        "A continuation was rejected.");

    // Still speaking: not ready yet.
    Check(!arbiter.IsReady(start + std::chrono::milliseconds(900)),
        "The arbiter answered while the user was still talking.");
    Check(arbiter.IsReady(start + std::chrono::milliseconds(2200)),
        "The arbiter never became ready after the user stopped.");

    const std::string merged = arbiter.Take();
    Check(merged.find("can you check") != std::string::npos &&
        merged.find("the goal runner") != std::string::npos,
        "Merging lost part of the thought: " + merged);
    Check(arbiter.Size() == 0, "Taking the merged turn did not empty the queue.");
}

void TestArbiterIgnoresNoiseButNeverTypedInput()
{
    inputArbiterSettings settings;
    revia::agents::InputArbiter arbiter(settings);
    using revia::agents::InputSource;
    using revia::agents::InputVerdict;
    const auto now = std::chrono::system_clock::now();

    // Room noise and recogniser filler.
    Check(arbiter.Offer("uh", InputSource::Voice, now) == InputVerdict::IgnoredNoise,
        "Filler was treated as a question.");
    Check(arbiter.Offer("[BLANK_AUDIO]", InputSource::Voice, now) ==
        InputVerdict::IgnoredNoise,
        "A blank-audio marker was treated as input.");
    Check(arbiter.Offer("Thanks for watching!", InputSource::Voice, now) ==
        InputVerdict::IgnoredNoise,
        "A known recogniser hallucination was treated as input.");
    Check(arbiter.Offer("   ", InputSource::Voice, now) == InputVerdict::IgnoredEmpty,
        "Whitespace was treated as input.");

    // A short question is a real one.
    Check(arbiter.Offer("why?", InputSource::Voice, now) == InputVerdict::Queued,
        "A short question was discarded as noise.");

    // Typed input is never filtered, however short.
    revia::agents::InputArbiter typed(settings);
    Check(typed.Offer("ok", InputSource::Typed, now) == InputVerdict::Queued,
        "Deliberately typed input was filtered as noise.");
    Check(typed.Offer("uh", InputSource::Typed, now) == InputVerdict::Queued,
        "Typed input was subjected to the voice noise filter.");
}

void TestArbiterDropsRepeatsAndOverflow()
{
    inputArbiterSettings settings;
    settings.maxQueuedInputs = 3;
    revia::agents::InputArbiter arbiter(settings);
    using revia::agents::InputSource;
    using revia::agents::InputVerdict;
    const auto now = std::chrono::system_clock::now();

    Check(arbiter.Offer("open the report", InputSource::Voice, now) == InputVerdict::Queued,
        "The first utterance was rejected.");
    // Recognisers routinely emit one utterance twice.
    Check(arbiter.Offer("Open the report.", InputSource::Voice,
        now + std::chrono::milliseconds(200)) == InputVerdict::IgnoredDuplicate,
        "The same utterance was queued twice.");

    Check(arbiter.Offer("second", InputSource::Voice, now) == InputVerdict::Queued,
        "A distinct utterance was rejected.");
    Check(arbiter.Offer("third", InputSource::Voice, now) == InputVerdict::Queued,
        "A distinct utterance was rejected.");
    Check(arbiter.Offer("fourth", InputSource::Voice, now) == InputVerdict::DroppedOverflow,
        "The queue grew past its cap.");
}

} // namespace

int main(const int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--uia-inspect-notepad")
        {
            revia::actions::windows::WindowsAutomationExecutor executor;
            ActionRequest request;
            request.type = ActionType::InspectWindow;
            request.application = "notepad.exe";
            revia::actions::PolicyDecision decision;
            decision.verdict = PolicyVerdict::Allowed;
            const auto result = executor.Execute(request, decision);
            Check(result.succeeded && !result.entries.empty(),
                "Live Notepad UI Automation inspection returned no named controls.");
            request.type = ActionType::FocusWindow;
            const auto focus = executor.Execute(request, decision);
            Check(focus.succeeded, "Live Notepad UI Automation focus failed.");
            std::cout << "Notepad UI Automation integration passed with " <<
                result.entries.size() << " named controls.\n";
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--tts-live")
        {
            speechSettings settings;
            settings.bEnabled = true;
            std::atomic<bool> ready = false;
            std::atomic<bool> spoke = false;
            revia::speech::SpeechService service;
            service.Start(settings, [&](const revia::speech::SpeechEvent& event)
            {
                if (event.phase == "Ready" && event.elapsedMilliseconds < 0.0)
                {
                    ready.store(true);
                }
                if (event.phase == "Ready" && event.elapsedMilliseconds >= 0.0)
                {
                    spoke.store(true);
                }
            });
            for (int attempt = 0; attempt < 200 && !ready.load(); ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Check(ready.load(), "Windows SAPI did not become ready for live speech.");
            service.Speak("Revia voice test complete.", {});
            for (int attempt = 0; attempt < 1000 && !spoke.load(); ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            service.Shutdown();
            Check(spoke.load(), "Windows SAPI did not complete the live Revia utterance.");
            std::cout << "Revia TTS live integration passed.\n";
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--qwen-worker-live")
        {
            speechSettings settings;
            settings.pythonExecutable = "ThirdParty/QwenTTS/.venv/Scripts/python.exe";
            settings.qwenServiceScript = "Tools/qwen_tts_service.py";
            revia::speech::QwenTtsClient client;
            client.Configure(settings);
            std::string detail;
            Check(client.IsAvailable(detail),
                "The authenticated Qwen3-TTS worker did not become healthy: " + detail);
            client.Shutdown();
            std::cout << detail << '\n';
            return 0;
        }
#ifdef _WIN32
        if (argc > 1 && std::string(argv[1]) == "--uia-resolver-live")
        {
            RunVisionUiaLiveFixture();
            std::cout << "Vision-to-UIA live fixture passed.\n";
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--vision-action-notepad-live")
        {
            RunVisionActionNotepadLive();
            return 0;
        }
#endif
        TestPolicyBoundaries();
        TestParser();
        TestDesktopActionPolicy();
        TestConfigurationFailsClosed();
        TestDesktopActionRateLimitsAreDeterministic();
        TestDesktopRateLimitIsAudited();
        TestLlamaServerProcessRejectsMissingFiles();
        TestLlamaServerProcessStopIsBounded();
        TestStructuredLongTermMemory();
        TestLegacyMemoryMigration();
        TestBackgroundMemoryAgentLifecycle();
        TestDispatcherGate();
        TestFilesystemExecutorAndAudit();
        TestVisionActionParserFailsClosed();
        TestVisionResolverRequiresGeometryNameAndIdentity();
        TestRuntimeEventBus();
        TestAffectController();
        TestSpeechTextNormalization();
        TestSpeechRuntimePathResolution();
        TestVoicePresetPersistence();
        TestWindowsSpeechServiceInitialization();
        TestLocalApiKeys();
        TestGoalStoreRoundTrip();
        TestGoalRunnerRejectsUnverifiablePlan();
        TestGoalRunnerVerifiesSuccess();
        TestGoalRunnerStopsOnUnverifiableStep();
        TestGoalScopeCannotWidenAuthority();
        TestGoalResumesAfterRestart();
        TestGoalPlannerParsesMultiStepPlan();
        TestGoalPlannerRejectsUnusablePlans();
        TestGoalPlannerAcceptsRealModelOutput();
        TestPlannedDestructivePlanIsContained();
        TestPlannedGoalStillFacesValidation();
        TestPlannedGoalRunsEndToEnd();
        TestSandboxRehearsalLeavesRealFoldersAlone();
        TestSandboxRefusesPlansItCannotCopy();
        TestSandboxRehearsalCatchesABrokenPlan();
        TestDerivedGoalScopeCannotWiden();
        TestPerceptionIsOffByDefault();
        TestPerceptionExcludesSensitiveWindows();
        TestPerceptionSuppressesRatherThanRedacts();
        TestPerceptionCoalescesAndRateLimits();
        TestPerceptionMonitorStaysSilentWhenDisabled();
        TestActivityHistoryMergesAndSeparatesSpans();
        TestActivityHistoryStaysBounded();
        TestActivityHistoryAnswersTheStageQuestion();
        TestAttentionKeepsSilenceAsTheDefault();
        TestConversationStartersNeedARealTransition();
        TestConversationStartersRecognizeReturnAndSwitching();
        TestConversationStarterContinuesWithoutSlashCommands();
        TestAttentionHardSuppressions();
        TestAttentionCooldownsAndBudget();
        TestAttentionReducesItsOwnRateWhenWrong();
        TestProposalsCarryTheirEvidence();
        TestLearningNeedsEnoughEvidence();
        TestLearningDrawsTheUncomfortableConclusion();
        TestApprovedLessonIsMemoryNotPolicy();
        TestUnfinishedGoalOutranksAnObservation();
        TestControllerRespectsTheGateAndRecordsOutcomes();
        TestBargeInIgnoresReviaHearingHerself();
        TestBargeInNeedsSustainedSpeech();
        TestFragmenterStartsSpeakingBeforeGenerationEnds();
        TestFragmenterDoesNotCutMidClause();
        TestPostureReachesTheModel();
        TestConversationStyleRepairsAndVaries();
        TestConversationStyleRemovesOnlyStockTail();
        TestConversationContextKeepsCoherentRecentTurns();
        TestHardwarePlanScalesParallelLanesConservatively();
        TestInferenceSchedulerPrioritizesConversation();
        TestInferenceSchedulerPreemptsBackgroundForConversation();
        TestStreamedReplyIsNeverTruncated();
        TestArbiterMergesOneThought();
        TestArbiterIgnoresNoiseButNeverTypedInput();
        TestArbiterDropsRepeatsAndOverflow();
        std::cout << "All Revia foundation tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
