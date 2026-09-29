#include "Coding/acpClient.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

namespace revia::coding
{

namespace
{
using nlohmann::json;

constexpr std::size_t LongestFileRead = 2 * 1024 * 1024;
constexpr std::size_t LongestRawInput = 4000;
constexpr int InvalidParams = -32602;
constexpr int MethodNotFound = -32601;
constexpr int InternalError = -32603;
constexpr auto ReadSlice = std::chrono::milliseconds(50);
// After session/cancel the agent is given this long to answer the prompt as cancelled.
constexpr auto CancelGrace = std::chrono::seconds(8);

std::string Text(const json& value, const char* key)
{
    if (!value.is_object() || !value.contains(key)) return {};
    const json& field = value[key];
    return field.is_string() ? field.get<std::string>() : std::string();
}

bool StartsWith(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt)
    {
        // A trailing empty element comes from a root written with a final separator.
        if (rootIt->empty()) continue;
        if (candidateIt == candidate.end() || *candidateIt != *rootIt) return false;
    }
    return true;
}

std::string Sliced(const std::string& content, const json& params)
{
    const bool hasLine = params.contains("line") && params["line"].is_number_integer();
    const bool hasLimit = params.contains("limit") && params["limit"].is_number_integer();
    if (!hasLine && !hasLimit) return content;
    const long long firstLine = hasLine ? std::max<long long>(1, params["line"].get<long long>()) : 1;
    const long long limit = hasLimit ? std::max<long long>(0, params["limit"].get<long long>()) : -1;
    std::istringstream stream(content);
    std::string line;
    std::string out;
    long long number = 0;
    long long taken = 0;
    while (std::getline(stream, line))
    {
        ++number;
        if (number < firstLine) continue;
        if (limit >= 0 && taken >= limit) break;
        out += line;
        out += '\n';
        ++taken;
    }
    return out;
}
} // namespace

AcpClient::~AcpClient()
{
    Stop();
}

bool AcpClient::WithinWorkspace(
    const std::filesystem::path& workspace,
    const std::filesystem::path& path,
    std::string& outReason)
{
    if (workspace.empty())
    {
        outReason = "no workspace is set";
        return false;
    }
    if (!path.is_absolute())
    {
        outReason = "the path is not absolute";
        return false;
    }
    const std::filesystem::path root = std::filesystem::absolute(workspace).lexically_normal();
    const std::filesystem::path candidate = path.lexically_normal();
    if (!StartsWith(root, candidate))
    {
        outReason = "the path is outside the workspace";
        return false;
    }
    std::error_code error;
    if (std::filesystem::exists(candidate, error))
    {
        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
        const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(candidate, error);
        if (error)
        {
            outReason = "the path could not be resolved";
            return false;
        }
        if (!StartsWith(canonicalRoot, canonicalPath))
        {
            outReason = "the path leads outside the workspace through a link";
            return false;
        }
    }
    return true;
}

bool AcpClient::Start(const AcpLaunch& launch, std::string& outError)
{
    outError.clear();
    Stop();
    if (launch.workspace.empty() || !launch.workspace.is_absolute())
    {
        outError = "The coding agent workspace must be an absolute path.";
        return false;
    }
    workspace = launch.workspace.lexically_normal();
    allowWrites = launch.allowWrites;
    core::StdioLaunch child;
    child.command = launch.command;
    child.arguments = launch.arguments;
    child.workingDirectory = workspace;
    child.logName = launch.logName;
    child.displayName = "coding agent";
    if (!process.Start(child, outError)) return false;
    started = true;

    json capabilities = {
        {"fs", {{"readTextFile", true}, {"writeTextFile", allowWrites}}},
        {"terminal", false}};
    const json params = {
        {"protocolVersion", ProtocolVersion},
        {"clientCapabilities", capabilities},
        {"clientInfo", {{"name", "revia"}, {"title", "Revia"}, {"version", "1"}}}};
    Pending pending;
    std::string result;
    if (!Call("initialize", params.dump(), result, outError, std::chrono::seconds(120), pending))
    {
        outError = "The coding agent did not complete initialization: " + outError;
        Stop();
        return false;
    }
    try
    {
        const json parsed = json::parse(result);
        if (!parsed.is_object() || !parsed.contains("protocolVersion") ||
            !parsed["protocolVersion"].is_number_integer() ||
            parsed["protocolVersion"].get<int>() != ProtocolVersion)
        {
            outError = "The coding agent speaks a protocol version this client does not (wanted " +
                std::to_string(ProtocolVersion) + ").";
            Stop();
            return false;
        }
        if (parsed.contains("agentInfo"))
        {
            agentName = Text(parsed["agentInfo"], "name");
            agentVersion = Text(parsed["agentInfo"], "version");
        }
    }
    catch (const std::exception& error)
    {
        outError = std::string("The coding agent's initialize reply was not readable: ") + error.what();
        Stop();
        return false;
    }
    if (agentName.empty()) agentName = "coding agent";
    return true;
}

bool AcpClient::NewSession(std::string& outSessionId, std::string& outError)
{
    outError.clear();
    if (!started)
    {
        outError = "The coding agent is not running.";
        return false;
    }
    const json params = {{"cwd", workspace.string()}, {"mcpServers", json::array()}};
    Pending pending;
    std::string result;
    if (!Call("session/new", params.dump(), result, outError, std::chrono::seconds(60), pending))
    {
        return false;
    }
    try
    {
        const json parsed = json::parse(result);
        outSessionId = Text(parsed, "sessionId");
    }
    catch (const std::exception& error)
    {
        outError = std::string("The coding agent's session reply was not readable: ") + error.what();
        return false;
    }
    if (outSessionId.empty())
    {
        outError = "The coding agent opened no session.";
        return false;
    }
    return true;
}

PromptResult AcpClient::Prompt(
    const std::string& sessionId,
    const std::string& text,
    const UpdateHandler& update,
    const PermissionHandler& permission,
    const std::stop_token stopToken,
    const std::chrono::seconds timeout,
    const std::size_t maximumMessageCharacters)
{
    PromptResult result;
    if (!started)
    {
        result.error = "The coding agent is not running.";
        return result;
    }
    Pending pending;
    pending.update = &update;
    pending.permission = &permission;
    pending.result = &result;
    pending.maximumMessageCharacters = maximumMessageCharacters;
    pending.stopToken = stopToken;
    pending.sessionId = sessionId;
    const json params = {
        {"sessionId", sessionId},
        {"prompt", json::array({{{"type", "text"}, {"text", text}}})}};
    std::string reply;
    std::string error;
    if (!Call("session/prompt", params.dump(), reply, error, timeout, pending))
    {
        if (pending.cancelSent)
        {
            result.cancelled = true;
            result.stopReason = "cancelled";
        }
        else
        {
            result.error = error;
        }
        return result;
    }
    try
    {
        const json parsed = json::parse(reply);
        result.stopReason = Text(parsed, "stopReason");
    }
    catch (const std::exception& parseError)
    {
        result.error = std::string("The coding agent's turn reply was not readable: ") + parseError.what();
        return result;
    }
    if (result.stopReason == "cancelled" || pending.cancelSent)
    {
        result.cancelled = true;
    }
    else if (result.stopReason == "end_turn" || result.stopReason == "max_tokens" ||
        result.stopReason == "max_turn_requests")
    {
        result.completed = true;
    }
    else if (result.stopReason == "refusal")
    {
        result.error = "The coding agent refused the task.";
    }
    else
    {
        result.error = "The coding agent stopped for a reason this client does not know: " +
            result.stopReason;
    }
    return result;
}

void AcpClient::Stop()
{
    process.Stop();
    started = false;
}

void AcpClient::Notify(const std::string& method, const std::string& params)
{
    const std::string line = "{\"jsonrpc\":\"2.0\",\"method\":" + json(method).dump() +
        ",\"params\":" + params + "}\n";
    (void)process.Write(line);
}

bool AcpClient::Call(
    const std::string& method,
    const std::string& params,
    std::string& outResult,
    std::string& outError,
    const std::chrono::milliseconds timeout,
    Pending& pending)
{
    const std::uint64_t id = nextId++;
    const std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":" + json(method).dump() + ",\"params\":" + params + "}\n";
    if (!process.Write(line))
    {
        outError = "The coding agent's input is closed.";
        return false;
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (!pending.cancelSent && !pending.sessionId.empty() && pending.stopToken.stop_requested())
        {
            Notify("session/cancel", json({{"sessionId", pending.sessionId}}).dump());
            pending.cancelSent = true;
            deadline = std::min(deadline, std::chrono::steady_clock::now() + CancelGrace);
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            outError = pending.cancelSent
                ? "The coding agent did not acknowledge the cancellation."
                : "The coding agent did not answer " + method + " in time.";
            return false;
        }
        std::string received;
        bool closed = false;
        if (!process.ReadLine(received, ReadSlice, closed))
        {
            if (closed)
            {
                outError = "The coding agent closed its output" +
                    std::string(process.IsRunning() ? "." : " and exited.");
                return false;
            }
            continue;
        }
        if (received.empty()) continue;
        json message;
        try
        {
            message = json::parse(received);
        }
        catch (const std::exception&)
        {
            // Not JSON: a stray line on stdout. Ignored rather than fatal, because an
            // agent that prints a banner is not an agent that is broken.
            continue;
        }
        if (!message.is_object()) continue;
        const bool isResponse = message.contains("id") && !message.contains("method");
        if (isResponse && message["id"].is_number_integer() &&
            message["id"].get<std::uint64_t>() == id)
        {
            if (message.contains("error"))
            {
                const json& fault = message["error"];
                outError = Text(fault, "message");
                if (outError.empty()) outError = "error " + fault.dump();
                if (fault.contains("data") && !fault["data"].is_null())
                {
                    outError += " (" + fault["data"].dump() + ")";
                }
                return false;
            }
            outResult = message.contains("result") ? message["result"].dump() : "null";
            return true;
        }
        HandleIncoming(received, pending);
    }
}

void AcpClient::HandleIncoming(const std::string& line, Pending& pending)
{
    json message;
    try
    {
        message = json::parse(line);
    }
    catch (const std::exception&)
    {
        return;
    }
    if (!message.is_object() || !message.contains("method")) return;
    const std::string method = Text(message, "method");
    const std::string params = message.contains("params") ? message["params"].dump() : "{}";

    if (message.contains("id"))
    {
        // The agent asking her something: it gets an answer whatever happens.
        std::string result;
        int code = 0;
        std::string errorMessage;
        std::string reply;
        if (AnswerAgentRequest(method, params, pending, result, code, errorMessage))
        {
            reply = "{\"jsonrpc\":\"2.0\",\"id\":" + message["id"].dump() + ",\"result\":" + result + "}\n";
        }
        else
        {
            reply = "{\"jsonrpc\":\"2.0\",\"id\":" + message["id"].dump() + ",\"error\":{\"code\":" +
                std::to_string(code) + ",\"message\":" + json(errorMessage).dump() + "}}\n";
        }
        (void)process.Write(reply);
        return;
    }

    if (method != "session/update" || pending.result == nullptr) return;
    json parsedParams;
    try
    {
        parsedParams = json::parse(params);
    }
    catch (const std::exception&)
    {
        return;
    }
    if (!parsedParams.is_object() || !parsedParams.contains("update") ||
        !parsedParams["update"].is_object())
    {
        return;
    }
    const json& raw = parsedParams["update"];
    const std::string kind = Text(raw, "sessionUpdate");
    AgentUpdate update;
    update.toolCallId = Text(raw, "toolCallId");
    update.title = Text(raw, "title");
    update.toolKind = Text(raw, "kind");
    update.status = Text(raw, "status");
    if (kind == "agent_message_chunk" || kind == "agent_thought_chunk")
    {
        update.kind = kind == "agent_message_chunk" ? AgentUpdate::Kind::MessageChunk
                                                    : AgentUpdate::Kind::ThoughtChunk;
        if (raw.contains("content") && raw["content"].is_object())
        {
            update.text = Text(raw["content"], "text");
        }
        if (update.kind == AgentUpdate::Kind::MessageChunk && !update.text.empty())
        {
            std::string& message = pending.result->message;
            if (message.size() < pending.maximumMessageCharacters)
            {
                message += update.text;
                if (message.size() > pending.maximumMessageCharacters)
                {
                    message = utf8::Prefix(message, pending.maximumMessageCharacters);
                }
            }
        }
    }
    else if (kind == "tool_call")
    {
        update.kind = AgentUpdate::Kind::ToolCall;
        ++pending.result->toolCalls;
    }
    else if (kind == "tool_call_update")
    {
        update.kind = AgentUpdate::Kind::ToolCallUpdate;
    }
    else if (kind == "plan")
    {
        update.kind = AgentUpdate::Kind::Plan;
        if (raw.contains("entries") && raw["entries"].is_array())
        {
            for (const json& entry : raw["entries"])
            {
                if (!entry.is_object()) continue;
                if (!update.text.empty()) update.text += "\n";
                update.text += Text(entry, "content");
                const std::string status = Text(entry, "status");
                const std::string priority = Text(entry, "priority");
                if (!status.empty() || !priority.empty())
                {
                    update.text += " [" + status + (priority.empty() ? "" : ", " + priority) + "]";
                }
            }
        }
    }
    else
    {
        update.kind = AgentUpdate::Kind::Other;
        update.text = kind;
    }
    if (pending.update != nullptr && *pending.update) (*pending.update)(update);
}

bool AcpClient::AnswerAgentRequest(
    const std::string& method,
    const std::string& params,
    Pending& pending,
    std::string& outResult,
    int& outErrorCode,
    std::string& outErrorMessage)
{
    json parsed;
    try
    {
        parsed = json::parse(params);
    }
    catch (const std::exception&)
    {
        outErrorCode = InvalidParams;
        outErrorMessage = "The request's params were not JSON.";
        return false;
    }
    if (!parsed.is_object()) parsed = json::object();
    const auto refused = [&](const int code, const std::string& why)
    {
        if (pending.result != nullptr) ++pending.result->refusedRequests;
        outErrorCode = code;
        outErrorMessage = why;
        return false;
    };

    if (method == "session/request_permission")
    {
        PermissionRequest request;
        if (parsed.contains("toolCall") && parsed["toolCall"].is_object())
        {
            const json& call = parsed["toolCall"];
            request.toolCallId = Text(call, "toolCallId");
            request.title = Text(call, "title");
            request.toolKind = Text(call, "kind");
            if (call.contains("rawInput") && !call["rawInput"].is_null())
            {
                request.rawInput = utf8::Prefix(call["rawInput"].dump(), LongestRawInput);
            }
        }
        if (parsed.contains("options") && parsed["options"].is_array())
        {
            for (const json& option : parsed["options"])
            {
                if (!option.is_object()) continue;
                request.options.push_back({Text(option, "optionId"), Text(option, "name"), Text(option, "kind")});
            }
        }
        if (pending.result != nullptr) ++pending.result->permissionsAsked;
        PermissionAnswer answer = PermissionAnswer::Cancelled;
        if (pending.stopToken.stop_requested())
        {
            answer = PermissionAnswer::Cancelled;
        }
        else if (pending.permission != nullptr && *pending.permission)
        {
            answer = (*pending.permission)(request);
        }
        else
        {
            // Nobody can consent, so nothing is consented to.
            answer = PermissionAnswer::RejectOnce;
        }
        const auto option = [&](std::initializer_list<const char*> kinds) -> std::string
        {
            for (const char* wanted : kinds)
            {
                for (const PermissionOption& candidate : request.options)
                {
                    if (candidate.kind == wanted) return candidate.id;
                }
            }
            return {};
        };
        std::string chosen;
        switch (answer)
        {
            case PermissionAnswer::AllowOnce: chosen = option({"allow_once", "allow_always"}); break;
            case PermissionAnswer::AllowAlways: chosen = option({"allow_always", "allow_once"}); break;
            case PermissionAnswer::RejectOnce: chosen = option({"reject_once", "reject_always"}); break;
            case PermissionAnswer::RejectAlways: chosen = option({"reject_always", "reject_once"}); break;
            case PermissionAnswer::Cancelled: break;
        }
        if (chosen.empty())
        {
            outResult = json({{"outcome", {{"outcome", "cancelled"}}}}).dump();
        }
        else
        {
            outResult = json({{"outcome", {{"outcome", "selected"}, {"optionId", chosen}}}}).dump();
        }
        return true;
    }

    if (method == "fs/read_text_file" || method == "fs/write_text_file")
    {
        const std::string pathText = Text(parsed, "path");
        if (pathText.empty()) return refused(InvalidParams, "The request named no path.");
        const std::filesystem::path path = actions::Utf8ToPath(pathText);
        std::string reason;
        if (!WithinWorkspace(workspace, path, reason))
        {
            return refused(InvalidParams, "Refused: " + reason + " (" + workspace.string() + ").");
        }
        std::error_code error;
        if (method == "fs/read_text_file")
        {
            if (!std::filesystem::is_regular_file(path, error))
            {
                return refused(InvalidParams, "Refused: the file does not exist.");
            }
            if (std::filesystem::file_size(path, error) > LongestFileRead)
            {
                return refused(InvalidParams, "Refused: the file is larger than this client reads.");
            }
            std::ifstream stream(path, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (pending.result != nullptr) ++pending.result->filesRead;
            outResult = json({{"content", Sliced(content, parsed)}}).dump();
            return true;
        }
        if (!allowWrites) return refused(InvalidParams, "Refused: writes are off for this agent.");
        if (!parsed.contains("content") || !parsed["content"].is_string())
        {
            return refused(InvalidParams, "The request carried no content.");
        }
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << parsed["content"].get<std::string>();
        stream.flush();
        if (!stream.good()) return refused(InternalError, "The file could not be written.");
        if (pending.result != nullptr) ++pending.result->filesWritten;
        outResult = "{}";
        return true;
    }

    if (method.rfind("terminal/", 0) == 0)
    {
        return refused(MethodNotFound, "Terminals are not offered by this client.");
    }
    return refused(MethodNotFound, "This client does not answer " + method + ".");
}

} // namespace revia::coding
