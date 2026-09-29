#include "reviaSessionTestAccess.h"
#include "Coding/acpClient.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>

namespace
{
using revia::coding::AcpClient;
using revia::coding::AcpLaunch;
using revia::coding::AgentUpdate;
using revia::coding::PermissionAnswer;
using revia::coding::PermissionRequest;
using revia::coding::PromptResult;
using revia::runtime::ReviaSession;
using revia::runtime::RuntimeEvent;
using revia::runtime::RuntimeEventKind;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::filesystem::path FakeAgentScript()
{
    return std::filesystem::path(__FILE__).parent_path() / "fakeAcpAgent.py";
}

const char* PythonCommand()
{
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

AcpLaunch FakeLaunch(const std::filesystem::path& workspace)
{
    AcpLaunch launch;
    launch.command = PythonCommand();
    launch.arguments = {FakeAgentScript().string()};
    launch.workspace = std::filesystem::absolute(workspace);
    launch.logName = "fake-coding-agent";
    return launch;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

struct Recorded
{
    std::vector<AgentUpdate> updates;
    std::vector<PermissionRequest> asked;
};

PromptResult Run(AcpClient& agent, const std::string& task, PermissionAnswer answer, Recorded& recorded,
    std::stop_token stop = {}, std::function<void(const AgentUpdate&)> onUpdate = {})
{
    std::string sessionId;
    std::string error;
    Check(agent.NewSession(sessionId, error) && sessionId == "sess_fake_1",
        "The fake agent opened no session: " + error);
    return agent.Prompt(sessionId, task,
        [&](const AgentUpdate& update)
        {
            recorded.updates.push_back(update);
            if (onUpdate) onUpdate(update);
        },
        [&](const PermissionRequest& request)
        {
            recorded.asked.push_back(request);
            return answer;
        },
        stop, std::chrono::seconds(30));
}

// The fake agent needs a Python. Where the tests run there is one; on a machine
// without it the suite says so rather than pretending to have run.
bool StartOrSkip(AcpClient& agent, const AcpLaunch& launch)
{
    std::string error;
    if (agent.Start(launch, error)) return true;
#ifdef _WIN32
    std::cout << "Coding agent tests skipped: " << error << "\n";
    return false;
#else
    Check(false, "The fake coding agent did not start: " + error);
    return false;
#endif
}

void TestTheHandshakeAndAPlainAnswer()
{
    revia::tests::ScopedTestDirectory directory;
    AcpClient agent;
    if (!StartOrSkip(agent, FakeLaunch(directory.root))) return;
    Check(agent.AgentName() == "fake-acp" && agent.AgentVersion() == "0.1",
        "The agent's name and version were not read from initialize: " + agent.AgentName());
    Recorded recorded;
    const PromptResult result = Run(agent, "say hello", PermissionAnswer::RejectOnce, recorded);
    Check(result.completed && result.stopReason == "end_turn" && result.error.empty(),
        "A plain turn did not complete: " + result.error + " (" + result.stopReason + ")");
    Check(result.message == "Hello from the fake agent.",
        "The agent's chunks were not gathered in order: '" + result.message + "'");
    std::size_t thoughts = 0;
    std::size_t chunks = 0;
    for (const AgentUpdate& update : recorded.updates)
    {
        if (update.kind == AgentUpdate::Kind::ThoughtChunk) ++thoughts;
        if (update.kind == AgentUpdate::Kind::MessageChunk) ++chunks;
    }
    Check(thoughts == 1 && chunks == 2, "The updates were not streamed as they arrived.");
    Check(recorded.asked.empty() && result.permissionsAsked == 0, "A plain answer asked permission.");
    agent.Stop();
    Check(!agent.IsRunning(), "The agent outlived Stop.");
}

void TestWritesGoThroughHerAndStayInTheWorkspace()
{
    revia::tests::ScopedTestDirectory directory;
    const std::filesystem::path workspace = std::filesystem::absolute(directory.root / "workspace");
    std::filesystem::create_directories(workspace);
    AcpClient agent;
    if (!StartOrSkip(agent, FakeLaunch(workspace))) return;

    // Allowed, inside: written through the client, then read back with line and limit.
    const std::filesystem::path inside = workspace / "notes" / "hello.txt";
    Recorded allowed;
    PromptResult result = Run(agent, "write " + inside.string() + " Hello there\nsecond line",
        PermissionAnswer::AllowOnce, allowed);
    Check(result.completed && result.error.empty(), "The write turn failed: " + result.error);
    Check(allowed.asked.size() == 1 && allowed.asked.front().title == "Write " + inside.string() &&
            allowed.asked.front().toolKind == "edit" && allowed.asked.front().options.size() == 3 &&
            Contains(allowed.asked.front().rawInput, "Hello there"),
        "The permission request did not carry the tool call and its options.");
    Check(ReadAll(inside) == "Hello there\nsecond line", "The file was not written where the agent asked.");
    Check(result.message == "Wrote it. First line reads: Hello there",
        "The read-back with line and limit did not return the first line: " + result.message);
    Check(result.toolCalls == 1 && result.permissionsAsked == 1 && result.filesWritten == 1 &&
            result.filesRead == 1 && result.refusedRequests == 0,
        "The turn's counts are wrong.");
    bool plan = false;
    bool completedCall = false;
    for (const AgentUpdate& update : allowed.updates)
    {
        if (update.kind == AgentUpdate::Kind::Plan && Contains(update.text, "Ask before writing")) plan = true;
        if (update.kind == AgentUpdate::Kind::ToolCallUpdate && update.status == "completed") completedCall = true;
    }
    Check(plan && completedCall, "The plan and the tool call's completion were not streamed.");

    // Allowed by the person, outside the workspace: refused by the client, and the
    // agent is told why.
    const std::filesystem::path outside = std::filesystem::absolute(directory.root / "escape.txt");
    Recorded escaped;
    result = Run(agent, "write " + outside.string() + " nope", PermissionAnswer::AllowAlways, escaped);
    Check(result.completed && Contains(result.message, "The write was refused") &&
            Contains(result.message, "outside the workspace"),
        "A write outside the workspace was not refused with its reason: " + result.message);
    Check(!std::filesystem::exists(outside) && result.refusedRequests == 1,
        "The file outside the workspace was written.");

    // A dotted path that lexically leaves the workspace is the same escape.
    const std::filesystem::path dotted = workspace / ".." / "dotted.txt";
    Recorded dottedRun;
    result = Run(agent, "write " + dotted.string() + " nope", PermissionAnswer::AllowOnce, dottedRun);
    Check(Contains(result.message, "outside the workspace") && !std::filesystem::exists(dotted.lexically_normal()),
        "A dotted escape was written.");

    // Declined: nothing written.
    const std::filesystem::path declined = workspace / "declined.txt";
    Recorded refused;
    result = Run(agent, "write " + declined.string() + " never", PermissionAnswer::RejectOnce, refused);
    Check(result.completed && result.message == "You declined, so nothing was written." &&
            !std::filesystem::exists(declined),
        "A declined write happened anyway: " + result.message);

    // A missing file read, and a terminal, are refused in words the agent can act on.
    Recorded missing;
    result = Run(agent, "read " + (workspace / "missing.txt").string(), PermissionAnswer::RejectOnce, missing);
    Check(Contains(result.message, "The read was refused") && Contains(result.message, "does not exist"),
        "A missing file was not refused plainly: " + result.message);
    Recorded terminal;
    result = Run(agent, "terminal", PermissionAnswer::AllowOnce, terminal);
    Check(result.completed && Contains(result.message, "No terminal") &&
            Contains(result.message, "not offered"),
        "A terminal was given, or refused without saying: " + result.message);
    Recorded refusal;
    result = Run(agent, "refuse", PermissionAnswer::AllowOnce, refusal);
    Check(!result.completed && !result.cancelled && Contains(result.error, "refused"),
        "An agent refusal was not reported: " + result.error);
}

void TestWritesCanBeSwitchedOff()
{
    revia::tests::ScopedTestDirectory directory;
    AcpLaunch launch = FakeLaunch(directory.root);
    launch.allowWrites = false;
    AcpClient agent;
    if (!StartOrSkip(agent, launch)) return;
    const std::filesystem::path target = launch.workspace / "readonly.txt";
    Recorded recorded;
    const PromptResult result = Run(agent, "write " + target.string() + " nope", PermissionAnswer::AllowOnce, recorded);
    Check(Contains(result.message, "writes are off") && !std::filesystem::exists(target),
        "A write happened with writes off: " + result.message);
}

void TestCancellationIsAcknowledged()
{
    revia::tests::ScopedTestDirectory directory;
    AcpClient agent;
    if (!StartOrSkip(agent, FakeLaunch(directory.root))) return;
    std::stop_source source;
    Recorded recorded;
    const auto started = std::chrono::steady_clock::now();
    const PromptResult result = Run(agent, "sleep", PermissionAnswer::RejectOnce, recorded, source.get_token(),
        [&](const AgentUpdate& update)
        {
            if (update.kind == AgentUpdate::Kind::MessageChunk) source.request_stop();
        });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Check(result.cancelled && result.stopReason == "cancelled" && !result.completed,
        "A stop did not cancel the turn: " + result.stopReason + " " + result.error);
    Check(elapsed < std::chrono::seconds(5), "The cancellation took too long to be acknowledged.");
    Check(Contains(result.message, "tick 0"), "Nothing streamed before the cancellation.");
}

void TestTheWorkspaceRuleOnItsOwn()
{
    revia::tests::ScopedTestDirectory directory;
    const std::filesystem::path workspace = std::filesystem::absolute(directory.root / "ws");
    std::filesystem::create_directories(workspace / "sub");
    std::string reason;
    Check(AcpClient::WithinWorkspace(workspace, workspace / "sub" / "file.txt", reason),
        "A path inside the workspace was refused: " + reason);
    Check(!AcpClient::WithinWorkspace(workspace, std::filesystem::path("relative/file.txt"), reason) &&
            Contains(reason, "not absolute"),
        "A relative path was accepted.");
    Check(!AcpClient::WithinWorkspace(workspace, workspace / ".." / "file.txt", reason) &&
            Contains(reason, "outside"),
        "A dotted escape was accepted.");
    Check(!AcpClient::WithinWorkspace(workspace, std::filesystem::absolute(directory.root / "wsx" / "f.txt"), reason),
        "A sibling whose name merely starts with the workspace's was accepted.");
#ifndef _WIN32
    const std::filesystem::path elsewhere = std::filesystem::absolute(directory.root / "elsewhere");
    std::filesystem::create_directories(elsewhere);
    std::error_code error;
    std::filesystem::create_directory_symlink(elsewhere, workspace / "link", error);
    if (!error)
    {
        std::ofstream(elsewhere / "secret.txt") << "x";
        Check(!AcpClient::WithinWorkspace(workspace, workspace / "link" / "secret.txt", reason) &&
                Contains(reason, "link"),
            "A link out of the workspace was followed.");
    }
#endif
}

void TestSheRunsTheAgentAsATask()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    const std::filesystem::path workspace = std::filesystem::absolute(directory.root / "code");
    std::mutex mutex;
    std::vector<RuntimeEvent> said;
    std::vector<std::string> phases;
    const auto id = session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        std::lock_guard lock(mutex);
        if (event.kind == RuntimeEventKind::AssistantMessage && event.component == "Coding agent")
        {
            said.push_back(event);
        }
        if (event.kind == RuntimeEventKind::ComponentStatus && event.component == "Coding agent")
        {
            phases.push_back(event.phase);
        }
    });
    const auto off = Access::SubmitOperator(session, "/code write something");
    Check(!off.succeeded && Contains(off.text, "No coding agent is set up"),
        "With no agent configured she did not say how to set one up: " + off.text);

    appSettings& settings = Access::Settings(session);
    settings.codingAgent.bEnabled = true;
    settings.codingAgent.command = PythonCommand();
    settings.codingAgent.arguments = {FakeAgentScript().string()};
    settings.codingAgent.workspace = workspace.string();
    std::vector<std::string> confirmed;
    session.SetConfirmationHandler([&](const revia::actions::ActionRequest& request,
        const revia::actions::PolicyDecision& decision)
    {
        std::lock_guard lock(mutex);
        confirmed.push_back(revia::actions::ToString(request.type) + " " + request.value + " " +
            request.application + " " + decision.reason);
        return revia::actions::ConfirmationChoice::Allow;
    });
    const std::filesystem::path target = workspace / "from-session.txt";
    const auto run = Access::SubmitOperator(session, "/code write " + target.string() + " Hi from Revia");
#ifdef _WIN32
    if (!run.succeeded && Contains(run.text, "did not start"))
    {
        std::cout << "Coding agent session test skipped: " << run.text << "\n";
        session.Events().Unsubscribe(id);
        return;
    }
#endif
    Check(run.succeeded && Contains(run.text, "succeeded"),
        "The coding task did not succeed through the session: " + run.text);
    Check(ReadAll(target) == "Hi from Revia", "The agent's write did not land in the workspace.");
    {
        std::lock_guard lock(mutex);
        Check(confirmed.size() == 1 && Contains(confirmed.front(), "agent_tool Write ") &&
                Contains(confirmed.front(), "fake-acp") && Contains(confirmed.front(), "only gate"),
            "The agent's permission request did not reach the confirmation handler as an agent tool.");
        Check(said.size() == 1 && Contains(said.front().message, "From fake-acp: Wrote it") &&
                Contains(said.front().detail, "quoted"),
            "The agent's report was not delivered as the agent's own, quoted words.");
        const auto has = [&](const char* phase)
        {
            return std::find(phases.begin(), phases.end(), phase) != phases.end();
        };
        Check(has("Connected") && has("Allowed") && has("Finished") && has("Tool"),
            "The activity feed did not show the agent's progress.");
    }
    session.Events().Unsubscribe(id);
}

} // namespace

void RunCodingAgentTests()
{
    TestTheWorkspaceRuleOnItsOwn();
    TestTheHandshakeAndAPlainAnswer();
    TestWritesGoThroughHerAndStayInTheWorkspace();
    TestWritesCanBeSwitchedOff();
    TestCancellationIsAcknowledged();
    TestSheRunsTheAgentAsATask();
    std::cout << "A hosted coding agent is driven over the Agent Client Protocol: its questions "
        "reach the person, its file access stays in the workspace, and a stop is honoured.\n";
}
