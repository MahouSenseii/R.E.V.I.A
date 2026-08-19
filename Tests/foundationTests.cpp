#include "Actions/IActionExecutor.h"
#include "Actions/actionDispatcher.h"
#include "Actions/actionTypes.h"
#include "Agents/memoryAgent.h"
#include "Core/localApiKey.h"
#include "Audit/actionAuditLogger.h"
#include "Filesystem/fileSystemExecutor.h"
#include "Goals/goalRunner.h"
#include "Goals/goalSandbox.h"
#include "Goals/goalStore.h"
#include "Goals/goalTypes.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "Memory/longTermMemory.h"
#include "Perception/activityHistory.h"
#include "Perception/windowEventMonitor.h"
#include "Planning/goalPlanner.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityPolicy.h"
#include "Policy/permissionStore.h"
#include "Runtime/affectController.h"
#include "Runtime/runtimeEvents.h"
#include "Speech/speechService.h"
#include "Speech/speechRecognitionService.h"
#include "Speech/qwenTtsClient.h"
#include "Speech/voicePresetStore.h"
#include "Windows/windowsAutomationExecutor.h"

#include <filesystem>
#include <atomic>
#include <fstream>
#include <chrono>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

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
}

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
        {"autoApproveRiskThrough", riskCeiling},
        {"createMissingApprovedRoots", false},
        {"maxReadBytes", 1048576},
        {"maxDirectoryEntries", 200},
        {"maxAffectedEntries", 200}
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
        TestPolicyBoundaries();
        TestParser();
        TestDesktopActionPolicy();
        TestConfigurationFailsClosed();
        TestLlamaServerProcessRejectsMissingFiles();
        TestLlamaServerProcessStopIsBounded();
        TestStructuredLongTermMemory();
        TestLegacyMemoryMigration();
        TestBackgroundMemoryAgentLifecycle();
        TestDispatcherGate();
        TestFilesystemExecutorAndAudit();
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
        std::cout << "All Revia foundation tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
