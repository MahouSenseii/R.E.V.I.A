#pragma once

#include "Core/stdioProcess.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::coding
{

// A coding agent she hosts rather than one she is.
//
// The Agent Client Protocol (agentclientprotocol.com) is how editors drive Claude Code,
// Codex, Gemini CLI and the rest: JSON-RPC over the agent's stdin and stdout, one line
// per message. Revia is the client. She starts the agent in its workspace, gives it the
// task, streams what it says, and answers what it asks -- and what it asks is the
// point. A permission request goes to the person through the same confirmation the
// rest of her actions use. A file read or write goes through her, confined to the
// workspace, so the agent's edits land where the person said and nowhere else. A
// terminal is not offered at all: the agent's own shell is the agent's, and this client
// never lends it hers.
struct AcpLaunch
{
    std::string command;
    std::vector<std::string> arguments;
    // Absolute. The session's cwd and the only place fs/read_text_file and
    // fs/write_text_file may reach.
    std::filesystem::path workspace;
    bool allowWrites = true;
    std::string logName = "coding-agent";
};

// One thing the agent said or did during a turn.
struct AgentUpdate
{
    enum class Kind
    {
        MessageChunk,
        ThoughtChunk,
        ToolCall,
        ToolCallUpdate,
        Plan,
        Other
    };
    Kind kind = Kind::Other;
    std::string text;
    std::string toolCallId;
    std::string title;
    std::string toolKind;
    std::string status;
};

struct PermissionOption
{
    std::string id;
    std::string name;
    // allow_once, allow_always, reject_once, reject_always.
    std::string kind;
};

struct PermissionRequest
{
    std::string toolCallId;
    std::string title;
    std::string toolKind;
    // The tool's input as the agent reported it, JSON, bounded. Shown, never acted on.
    std::string rawInput;
    std::vector<PermissionOption> options;
};

enum class PermissionAnswer
{
    AllowOnce,
    AllowAlways,
    RejectOnce,
    RejectAlways,
    Cancelled
};

struct PromptResult
{
    // The agent finished the turn on its own (stopReason end_turn or max_*).
    bool completed = false;
    bool cancelled = false;
    std::string stopReason;
    std::string error;
    // Everything the agent said, in order, bounded by the caller's limit.
    std::string message;
    std::size_t toolCalls = 0;
    std::size_t permissionsAsked = 0;
    std::size_t filesRead = 0;
    std::size_t filesWritten = 0;
    std::size_t refusedRequests = 0;
};

class AcpClient
{
public:
    using UpdateHandler = std::function<void(const AgentUpdate&)>;
    using PermissionHandler = std::function<PermissionAnswer(const PermissionRequest&)>;

    AcpClient() = default;
    ~AcpClient();
    AcpClient(const AcpClient&) = delete;
    AcpClient& operator=(const AcpClient&) = delete;

    // Starts the agent and completes `initialize`. One thread drives a client; every
    // call below is made from that thread.
    bool Start(const AcpLaunch& launch, std::string& outError);
    [[nodiscard]] const std::string& AgentName() const { return agentName; }
    [[nodiscard]] const std::string& AgentVersion() const { return agentVersion; }
    [[nodiscard]] bool IsRunning() const { return process.IsRunning(); }

    bool NewSession(std::string& outSessionId, std::string& outError);

    // One turn. Streams updates as they arrive and asks `permission` whenever the agent
    // does; a stop requested meanwhile sends session/cancel and waits for the agent to
    // acknowledge before giving up on it.
    PromptResult Prompt(
        const std::string& sessionId,
        const std::string& text,
        const UpdateHandler& update,
        const PermissionHandler& permission,
        std::stop_token stopToken,
        std::chrono::seconds timeout,
        std::size_t maximumMessageCharacters = 20000);

    void Stop();

    // The confinement rule, on its own: absolute, lexically inside the workspace, and,
    // where the path exists, canonically inside it too, so a link cannot lead out.
    [[nodiscard]] static bool WithinWorkspace(
        const std::filesystem::path& workspace,
        const std::filesystem::path& path,
        std::string& outReason);

    // What she answers on the agent's behalf, for the docs and the tests.
    static constexpr int ProtocolVersion = 1;

private:
    struct Pending
    {
        const UpdateHandler* update = nullptr;
        const PermissionHandler* permission = nullptr;
        PromptResult* result = nullptr;
        std::size_t maximumMessageCharacters = 0;
        std::stop_token stopToken;
        std::string sessionId;
        bool cancelSent = false;
    };

    // Sends one request and pumps until its response, answering the agent's own
    // requests and notifications meanwhile. Returns false with outError on a timeout,
    // a closed pipe or an error response.
    bool Call(
        const std::string& method,
        const std::string& params,
        std::string& outResult,
        std::string& outError,
        std::chrono::milliseconds timeout,
        Pending& pending);
    void Notify(const std::string& method, const std::string& params);
    // One incoming message that is not the response being waited for.
    void HandleIncoming(const std::string& line, Pending& pending);
    // The answer to an agent request, as a JSON result string, or false with an error.
    bool AnswerAgentRequest(
        const std::string& method,
        const std::string& params,
        Pending& pending,
        std::string& outResult,
        int& outErrorCode,
        std::string& outErrorMessage);

    core::StdioProcess process;
    std::filesystem::path workspace;
    bool allowWrites = true;
    bool started = false;
    std::uint64_t nextId = 1;
    std::string agentName;
    std::string agentVersion;
};

} // namespace revia::coding
