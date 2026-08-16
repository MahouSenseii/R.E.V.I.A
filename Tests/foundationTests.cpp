#include "Actions/IActionExecutor.h"
#include "Actions/actionDispatcher.h"
#include "Actions/actionTypes.h"
#include "Agents/memoryAgent.h"
#include "Core/localApiKey.h"
#include "Audit/actionAuditLogger.h"
#include "Filesystem/fileSystemExecutor.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "Memory/longTermMemory.h"
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
    Check(process.Start(settings, error),
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
    Check(store.Save(preset, error), "Voice preset save failed: " + error);
    Check(store.Assign("revia", preset.id, error), "Voice assignment failed: " + error);

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
        std::cout << "All Revia foundation tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
