#include "Agents/toolLoop.h"
#include "Agents/toolRegistry.h"
#include "Audit/contentDigest.h"
#include "Audit/mcpToolAudit.h"
#include "Core/appSettings.h"
#include "Core/configManager.h"
#include "Core/messageRouter.h"
#include "LLM/Cloud/cloudTransport.h"
#include "Mcp/mcpClient.h"
#include "Mcp/mcpManager.h"
#include "Mcp/mcpSchema.h"
#include "Mcp/mcpSettings.h"
#include "Mcp/mcpTransport.h"
#include "testSupport.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
using namespace revia;
using revia::tests::Check;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::filesystem::path FixturePath()
{
    if (const char* configured = std::getenv("REVIA_MCP_SERVER_FIXTURE"); configured && *configured)
        return configured;
#ifdef _WIN32
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    return std::filesystem::path(path).parent_path() / "ReviaMcpServerFixture.exe";
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path() / "ReviaMcpServerFixture";
#endif
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

mcpServerSettings FixtureServer(const std::vector<std::string>& extraArguments = {})
{
    mcpServerSettings server;
    server.id = "fake";
    server.bEnabled = true;
    server.transport = "stdio";
    server.command = actions::PathToUtf8(FixturePath());
    server.args = extraArguments;
    server.timeoutSeconds = 5;
    return server;
}

std::unique_ptr<mcp::McpClient> Connect(const mcpServerSettings& server, const std::chrono::milliseconds timeout = 5000ms)
{
    auto client = std::make_unique<mcp::McpClient>(mcp::MakeStdioTransport(server), timeout);
    std::string error;
    Check(client->Initialize({}, error), "The MCP fixture did not initialize: " + error);
    return client;
}

const agents::ChatTool* Find(const std::vector<agents::ChatTool>& tools, const std::string& name)
{
    const auto found = std::find_if(tools.begin(), tools.end(), [&](const auto& tool) { return tool.definition.name == name; });
    return found == tools.end() ? nullptr : &*found;
}

void TestSettingsAreValidatedAndOffByDefault()
{
    const mcpServerSettings defaults;
    Check(!defaults.bEnabled && defaults.bRequireConfirmation && defaults.allowedTools.empty(),
        "An MCP server must default to disabled, confirmed and offering no tools.");
    mcpServerSettings server = FixtureServer();
    Check(ValidateMcpServerSettings(server).empty(), "A valid stdio server was refused: " + ValidateMcpServerSettings(server));
    server.command = "relative/server";
    Check(!ValidateMcpServerSettings(server).empty(), "A relative command path was accepted.");
#ifdef _WIN32
    server.command = "C:\\Windows\\System32\\cmd.exe";
#else
    server.command = "/bin/sh";
#endif
    Check(!ValidateMcpServerSettings(server).empty(), "A shell was accepted as an MCP server command.");
    server = FixtureServer();
    server.id = "Bad Id";
    Check(!ValidateMcpServerSettings(server).empty(), "An invalid server id was accepted.");
    server = FixtureServer();
    server.timeoutSeconds = 0;
    Check(!ValidateMcpServerSettings(server).empty(), "A zero timeout was accepted.");
    server = FixtureServer();
    server.envAllowlist = {"PATH=evil"};
    Check(!ValidateMcpServerSettings(server).empty(), "An invalid environment name was accepted.");

    mcpServerSettings http;
    http.id = "remote";
    http.transport = "http";
    http.url = "http://example.com/mcp";
    Check(!ValidateMcpServerSettings(http).empty(), "Plain http to another host was accepted.");
    http.url = "http://127.0.0.1:8123/mcp";
    Check(ValidateMcpServerSettings(http).empty(), "Loopback http was refused.");
    http.url = "https://mcp.example.com/v1";
    Check(ValidateMcpServerSettings(http).empty(), "An https server was refused.");
    http.command = "/usr/bin/server";
    Check(!ValidateMcpServerSettings(http).empty(), "An http server with a command was accepted.");

    // The shipped settings configure no server, and configManager refuses a bad entry.
    tests::ScopedTestDirectory directory;
    std::ifstream shipped("Config/settings.json");
    Check(shipped.good(), "Run the MCP suite from the repository root so it can read Config/settings.json.");
    Json settings = Json::parse(shipped);
    Check(settings.contains("mcp") && settings["mcp"]["servers"].is_array() && settings["mcp"]["servers"].empty(),
        "Config/settings.json must ship an empty mcp.servers list.");
    const auto load = [&](const Json& value, appSettings& out)
    {
        const auto path = directory.root / "settings.json";
        std::ofstream(path) << value.dump(2);
        return configManager(actions::PathToUtf8(path), actions::PathToUtf8(directory.root / "Profiles")).LoadSettings(out);
    };
    appSettings loaded;
    Check(load(settings, loaded) && loaded.mcp.servers.empty(), "The shipped settings did not load.");
    settings["mcp"]["servers"] = Json::array({{{"id", "fake"}, {"transport", "stdio"}, {"command", server.command}, {"args", {"--x"}},
        {"allowedTools", {"echo"}}, {"timeoutSeconds", 7}}});
    appSettings parsed;
    Check(load(settings, parsed) && parsed.mcp.servers.size() == 1, "A valid MCP server entry did not load.");
    const auto& entry = parsed.mcp.servers.front();
    Check(!entry.bEnabled && entry.bRequireConfirmation && entry.timeoutSeconds == 7 && entry.allowedTools == std::vector<std::string>{"echo"} &&
              entry.args == std::vector<std::string>{"--x"},
        "An MCP server entry was not parsed with its safe defaults.");
    settings["mcp"]["servers"][0]["command"] = "server.exe";
    appSettings refused;
    Check(!load(settings, refused), "configManager accepted an MCP server with a relative command.");
    settings["mcp"]["servers"] = Json::array({{{"id", "a"}, {"transport", "stdio"}, {"command", server.command}},
        {{"id", "a"}, {"transport", "stdio"}, {"command", server.command}}});
    Check(!load(settings, refused), "configManager accepted two MCP servers with the same id.");
}

void TestSchemasAreClosedOrRefused()
{
    std::string error;
    const std::string schema = mcp::NormalizeInputSchema(
        {{"type", "object"}, {"properties", {{"text", {{"type", "string"}, {"title", "T"}, {"format", "uri"}}}, {"count", {{"type", "integer"}, {"minimum", 1}}}}},
            {"required", {"text"}}},
        error);
    Check(!schema.empty(), "A plain object schema was refused: " + error);
    const Json normalized = Json::parse(schema);
    Check(normalized["additionalProperties"] == false && normalized["properties"]["text"]["maxLength"] == 4096 &&
              !normalized["properties"]["text"].contains("format"),
        "A normalized schema was not closed and bounded.");
    Json arguments;
    Check(agents::ValidateToolArguments(schema, R"({"text":"hi","count":2})", arguments, error), "Valid arguments were refused: " + error);
    Check(!agents::ValidateToolArguments(schema, R"({"text":"hi","extra":1})", arguments, error), "An unnamed argument was accepted.");
    Check(!agents::ValidateToolArguments(schema, R"({"text":"hi","count":0})", arguments, error), "An out-of-range integer was accepted.");

    const std::string listed = mcp::NormalizeInputSchema({{"type", "object"},
                                                             {"properties", {{"ratio", {{"type", "number"}, {"maximum", 1.5}}},
                                                                                {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 2}}}}}},
        error);
    Check(!listed.empty(), "Numbers and arrays were refused: " + error);
    Check(agents::ValidateToolArguments(listed, R"({"ratio":0.5,"tags":["a","b"]})", arguments, error), "A valid list was refused: " + error);
    Check(!agents::ValidateToolArguments(listed, R"({"tags":["a","b","c"]})", arguments, error), "A list past maxItems was accepted.");
    Check(!agents::ValidateToolArguments(listed, R"({"ratio":2})", arguments, error), "A number past its maximum was accepted.");
    Check(!agents::ValidateToolArguments(listed, R"({"tags":[1]})", arguments, error), "A list item of the wrong type was accepted.");

    const auto refused = [&](const Json& value, const std::string& what)
    { Check(mcp::NormalizeInputSchema(value, error).empty(), "The schema with " + what + " was accepted."); };
    refused({{"type", "object"}, {"properties", {{"v", {{"anyOf", Json::array({{{"type", "string"}}})}}}}}}, "anyOf");
    refused({{"type", "object"}, {"additionalProperties", true}}, "open additionalProperties");
    refused({{"type", "object"}, {"properties", {{"v", {{"type", "string"}, {"pattern", "^a"}}}}}}, "a pattern");
    refused({{"type", "object"}, {"properties", {{"v", {{"type", Json::array({"string", "null"})}}}}}}, "a list of types");
    refused({{"type", "object"}, {"properties", {{"v", {{"$ref", "#/defs/x"}}}}}}, "$ref");
    refused({{"type", "object"}, {"required", {"missing"}}}, "a required key it does not describe");
    refused({{"type", "string"}}, "a non-object root");
    refused({{"type", "object"}, {"properties", {{"v", {{"type", "array"}}}}}}, "an array without items");
    Json deep = {{"type", "string"}};
    for (int level = 0; level < 5; ++level)
        deep = {{"type", "object"}, {"properties", {{"x", deep}}}};
    refused(deep, "five nested objects");
    Json allowedDepth = {{"type", "string"}};
    for (int level = 0; level < 4; ++level)
        allowedDepth = {{"type", "object"}, {"properties", {{"x", allowedDepth}}}};
    const std::string fourLevels = mcp::NormalizeInputSchema(allowedDepth, error);
    Check(!fourLevels.empty() && agents::ValidateToolArguments(fourLevels, R"({"x":{"x":{"x":{"x":"leaf"}}}})", arguments, error),
        "Four nested levels must be accepted by both the normalizer and the argument validator: " + error);

    Check(mcp::ChatToolName("fs", "read.file") == "mcp_fs_read_file", "A dotted tool name was not namespaced.");
    Check(mcp::ChatToolName("fs", "read/file").empty(), "A tool name with a slash was offered.");
    Check(mcp::ChatToolName("fs", std::string(70, 'a')).empty(), "A function name past 64 characters was offered.");
}

void TestHandshakeListAndCall()
{
    tests::ScopedTestDirectory directory;
    const auto log = directory.root / "calls.log";
    auto client = Connect(FixtureServer({"--log", actions::PathToUtf8(log), "--page-size", "3"}));
    Check(client->ServerName() == "revia-fixture" && client->NegotiatedVersion() == mcp::ProtocolVersion,
        "The handshake did not record the server's name and revision.");
    std::vector<mcp::McpToolInfo> tools;
    std::string error;
    Check(client->ListTools(tools, {}, error), "tools/list failed: " + error);
    Check(tools.size() == 15 && tools.front().name == "echo" && tools.back().name == "inject",
        "tools/list did not follow every page: " + std::to_string(tools.size()));
    const std::string logged = ReadAll(log);
    Check(Contains(logged, "initialize ") && Contains(logged, "\"capabilities\":{}") && Contains(logged, "\"name\":\"Revia\"") &&
              Contains(logged, "\"protocolVersion\":\"2025-06-18\"") && Contains(logged, "\ninitialized"),
        "The initialize handshake was not what the protocol requires: " + logged);

    const auto echo = client->CallTool("echo", {{"text", "hi"}}, {});
    Check(echo.answered && !echo.toolError && echo.text == "echo: hi" && echo.resultBytes > 0, "tools/call did not return the text.");
    const auto fail = client->CallTool("fail", Json::object(), {});
    Check(fail.answered && fail.toolError && fail.text == "boom", "A tool error was not reported as one.");
    const auto unknown = client->CallTool("nope", Json::object(), {});
    Check(!unknown.answered && Contains(unknown.error, "-32602"), "A JSON-RPC error was not surfaced: " + unknown.error);

    // The server's own ping during tools/list was answered, and its sampling and
    // elicitation requests were refused as methods Revia does not have.
    const auto sample = client->CallTool("sample", Json::object(), {});
    Check(sample.answered && sample.text == "sampling=-32601;elicitation=-32601",
        "A server-initiated sampling or elicitation request was not refused: " + sample.text + sample.error);
    const auto report = Json::parse(client->CallTool("report", Json::object(), {}).text);
    Check(report["pingReply"].contains("result") && report["pingReply"]["result"] == Json::object(), "The server's ping was not answered.");

    const auto rich = client->CallTool("rich", Json::object(), {});
    Check(Contains(rich.text, "hello") && Contains(rich.text, "An image (image/png) was returned and not shown") &&
              Contains(rich.text, "Resource link: today.md <file:///notes/today.md>") && Contains(rich.text, "inline text") &&
              !Contains(rich.text, "iVBORw0KGgo"),
        "Mixed content was not reduced to text: " + rich.text);
    const auto big = client->CallTool("big", Json::object(), {});
    Check(big.answered && big.text.size() <= mcp::McpClient::MaximumResultTextBytes && Contains(big.text, "[Result truncated by Revia.]") &&
              big.resultBytes > 100000,
        "A large result was not bounded.");
    // One message past the transport's limit stops the server rather than growing Revia.
    const auto huge = client->CallTool("huge", Json::object(), {});
    Check(!huge.answered && Contains(huge.error, "larger than Revia accepts") && !client->Connected(),
        "An oversized message was not refused: " + huge.error);
}

void TestTimeoutCancellationAndListChanged()
{
    auto client = Connect(FixtureServer(), 300ms);
    const auto started = std::chrono::steady_clock::now();
    const auto slow = client->CallTool("slow", Json::object(), {});
    Check(!slow.answered && slow.timedOut && std::chrono::steady_clock::now() - started < 3s, "An unanswered call did not time out.");
    std::stop_source stop;
    std::jthread stopper([&stop] { std::this_thread::sleep_for(100ms); stop.request_stop(); });
    const auto stopped = client->CallTool("slow", Json::object(), stop.get_token());
    Check(!stopped.answered && stopped.cancelled, "A stopped call was not cancelled.");
    const auto report = Json::parse(client->CallTool("report", Json::object(), {}).text);
    Check(report["cancelled"].size() == 2, "notifications/cancelled was not sent for each abandoned request: " + report.dump());
    Check(client->Connected(), "Abandoned requests must not end the connection.");

    std::vector<mcp::McpToolInfo> tools;
    std::string error;
    Check(client->ListTools(tools, {}, error) && !client->ToolsChanged(), "The first list failed.");
    Check(client->CallTool("change", Json::object(), {}).text == "changed", "The change tool failed.");
    for (int attempt = 0; attempt < 50 && !client->ToolsChanged(); ++attempt)
    {
        client->Poll();
        std::this_thread::sleep_for(10ms);
    }
    Check(client->ToolsChanged(), "notifications/tools/list_changed was not noticed.");
    Check(client->ListTools(tools, {}, error) && tools.back().name == "added" && !client->ToolsChanged(),
        "The changed tool list was not read again.");

    // A revision Revia does not speak, and a server that dies at once.
    mcp::McpClient wrong(mcp::MakeStdioTransport(FixtureServer({"--version", "1999-01-01"})), 2000ms);
    Check(!wrong.Initialize({}, error) && Contains(error, "1999-01-01"), "A server with an unknown revision was accepted.");
    mcp::McpClient dead(mcp::MakeStdioTransport(FixtureServer({"--exit-on-start"})), 2000ms);
    Check(!dead.Initialize({}, error) && Contains(error, "fixture refused to start"), "A server that exited was not diagnosed: " + error);
    mcpServerSettings missing = FixtureServer();
    missing.command = actions::PathToUtf8(FixturePath().parent_path() / "no-such-mcp-server");
    mcp::McpClient absent(mcp::MakeStdioTransport(missing), 2000ms);
    Check(!absent.Initialize({}, error), "A missing server program was reported as started.");
}

// A server that exits at once is noticed -- by a failed write, or by the end of its
// output -- a moment before it has fully gone and its stderr has been read. The reason
// must still be reported, and an error left over from an earlier call must not be.
class ExitingTransport final : public mcp::McpTransport
{
  public:
    // Where the exit is first seen: before the first request, at its write, or as the
    // end of output while the answer is awaited.
    enum class Seen
    {
        BeforeRequest,
        AtWrite,
        AwaitingAnswer
    };
    explicit ExitingTransport(const Seen seen) : seen(seen)
    {
    }
    bool Open(std::string&) override
    {
        opened = std::chrono::steady_clock::now();
        return true;
    }
    bool Send(const std::string&, std::stop_token, std::string& error) override
    {
        sent = true;
        if (seen != Seen::AtWrite)
            return true;
        error = "The server stopped reading its input.";
        return false;
    }
    std::optional<std::string> Receive(const std::chrono::milliseconds wait) override
    {
        std::this_thread::sleep_for(std::min(wait, std::chrono::milliseconds(5)));
        return std::nullopt;
    }
    bool Closed() const override
    {
        if (seen == Seen::BeforeRequest)
            return true;
        return sent && Since() >= (seen == Seen::AtWrite ? 50ms : 0ms);
    }
    std::string Diagnostics() const override
    {
        return Since() >= 150ms ? "Its last error output: fixture refused to start" : "";
    }
    void Close() override
    {
    }

  private:
    std::chrono::milliseconds Since() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened);
    }
    Seen seen;
    bool sent = false;
    std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
};

void TestServerThatExitsAtOnceIsDiagnosed()
{
    using Seen = ExitingTransport::Seen;
    for (const Seen seen : {Seen::BeforeRequest, Seen::AtWrite, Seen::AwaitingAnswer})
    {
        mcp::McpClient client(std::make_unique<ExitingTransport>(seen), 2000ms);
        std::string error = "The server speaks protocol revision \"1999-01-01\", which Revia does not.";
        const bool initialized = client.Initialize({}, error);
        Check(!initialized && Contains(error, "fixture refused to start") && !Contains(error, "1999-01-01"),
            "A server that exited at once (case " + std::to_string(static_cast<int>(seen)) + ") was not diagnosed: " + error);
    }
}

struct GateRecorder
{
    std::mutex mutex;
    std::vector<audit::McpToolAuditEntry> audit;
    std::vector<mcp::McpConfirmationRequest> asked;
    bool auditWorks = true;
    bool approve = true;

    mcp::McpGatePorts Ports(const bool withConfirm)
    {
        mcp::McpGatePorts ports;
        ports.audit = [this](const audit::McpToolAuditEntry& entry)
        {
            std::lock_guard lock(mutex);
            audit.push_back(entry);
            return auditWorks;
        };
        if (withConfirm)
        {
            ports.confirm = [this](const mcp::McpConfirmationRequest& request)
            {
                std::lock_guard lock(mutex);
                asked.push_back(request);
                return approve;
            };
        }
        return ports;
    }
};

void TestManagerGatesEveryCall()
{
    tests::ScopedTestDirectory directory;
    const auto log = directory.root / "calls.log";
    mcpServerSettings server = FixtureServer({"--log", actions::PathToUtf8(log)});
    server.allowedTools = {"echo", "numbers", "anyof", "open", "deep", "fail", "inject", "not-on-server"};
    mcpSettings settings;
    settings.servers = {server};
    mcp::McpManager manager;
    manager.Configure(settings);
    GateRecorder recorder;

    Check(manager.ChatTools(false, recorder.Ports(true), {}).empty(), "A non-private audience was offered MCP tools.");
    Check(manager.Status().front().state == "not started", "A non-private turn started an MCP server.");
    Check(manager.ChatTools(true, mcp::McpGatePorts{}, {}).empty(), "MCP tools were offered with no audit.");

    auto tools = manager.ChatTools(true, recorder.Ports(false), {});
    std::vector<std::string> names;
    for (const auto& tool : tools)
        names.push_back(tool.definition.name);
    Check(names == std::vector<std::string>{"mcp_fake_echo", "mcp_fake_fail", "mcp_fake_numbers", "mcp_fake_inject"},
        "The offered tools were not exactly the allowlisted ones with strict schemas.");
    const auto status = manager.Status().front();
    const auto withheld = [&](const std::string& prefix)
    { return std::any_of(status.withheld.begin(), status.withheld.end(), [&](const auto& line) { return line.rfind(prefix, 0) == 0; }); };
    Check(status.state == "connected" && withheld("anyof: its input schema was refused") && withheld("open: its input schema") &&
              withheld("deep: its input schema") && withheld("secret: not in allowedTools") && withheld("slow: not in allowedTools"),
        "Refused and unlisted tools were not reported: " + manager.StatusText());
    for (const auto& tool : tools)
        Check(tool.effect == agents::ToolEffect::Effect && !tool.parallelSafe, "An MCP tool was treated as harmless.");

    const auto* echo = Find(tools, "mcp_fake_echo");
    const agents::ToolCallContext context;
    const Json arguments = {{"text", "hello-secret-arg"}};

    // Confirmation required with nobody to ask: nothing reaches the server.
    auto outcome = echo->execute(arguments, context);
    Check(!outcome.succeeded && Contains(outcome.error, "needs your approval") && !Contains(ReadAll(log), "call echo"),
        "A call that needed approval ran without a prompt.");
    Check(recorder.audit.size() == 1 && recorder.audit.back().phase == "refused", "The refused call was not audited.");

    tools = manager.ChatTools(true, recorder.Ports(true), {});
    echo = Find(tools, "mcp_fake_echo");
    recorder.approve = false;
    outcome = echo->execute(arguments, context);
    Check(!outcome.succeeded && Contains(outcome.error, "declined") && !Contains(ReadAll(log), "call echo"), "A declined call ran.");

    recorder.approve = true;
    recorder.audit.clear();
    outcome = echo->execute(arguments, context);
    Check(outcome.succeeded && outcome.content == "echo: hello-secret-arg" && Contains(ReadAll(log), "call echo"), "An approved call did not run.");
    const auto& asked = recorder.asked.back();
    Check(asked.server == "fake" && asked.tool == "echo" && Contains(asked.arguments, "hello-secret-arg"),
        "The approval prompt did not say which server, tool and arguments.");
    Check(recorder.audit.size() == 2 && recorder.audit[0].phase == "intent" && recorder.audit[1].phase == "result" &&
              recorder.audit[1].succeeded && recorder.audit[1].confirmed && recorder.audit[1].resultBytes > 0 &&
              recorder.audit[0].transactionId == recorder.audit[1].transactionId &&
              recorder.audit[1].argumentsDigest == audit::ContentDigest(arguments.dump()) && recorder.audit[1].server == "fake" &&
              recorder.audit[1].tool == "echo",
        "The call was not audited with server, tool, digest and result size.");
    const std::string record = audit::McpToolAuditRecord(recorder.audit[1]);
    Check(!Contains(record, "hello-secret-arg") && Contains(record, "\"result_bytes\"") && Contains(record, "mcp_tool_call"),
        "The audit record copied the arguments or lost its size.");

    // An audit that cannot record stops the call before it is sent.
    recorder.auditWorks = false;
    const Json other = {{"text", "unrecorded"}};
    outcome = echo->execute(other, context);
    Check(!outcome.succeeded && !Contains(ReadAll(log), "unrecorded"), "A call ran although its audit failed.");
    recorder.auditWorks = true;

    // Tool errors and untrusted text.
    const auto failed = Find(tools, "mcp_fake_fail")->execute(Json::object(), context);
    Check(!failed.succeeded && Contains(failed.error, "boom"), "A tool error was not passed on as a failure.");
    const auto injected = Find(tools, "mcp_fake_inject")->execute(Json::object(), context);
    const std::string shown = agents::ToolLoop::FormatResult("mcp_fake_inject", injected, 4000);
    Check(injected.succeeded && Contains(shown, "[Untrusted output from the mcp_fake_inject tool") &&
              !Contains(shown, "[End of runtime context]"),
        "MCP output was not labelled and neutralized like other tool output: " + shown);

    // A turn that ended is never sent.
    std::stop_source ended;
    ended.request_stop();
    const agents::ToolCallContext stopped{ended.get_token(), {}, 1};
    Check(!echo->execute({{"text", "after-stop"}}, stopped).succeeded && !Contains(ReadAll(log), "after-stop"), "A stopped turn's call ran.");

    // The owner may turn confirmation off for one server; the audit stays.
    server.bRequireConfirmation = false;
    server.allowedTools = {"echo", "change", "added"};
    settings.servers = {server};
    manager.Configure(settings);
    recorder.asked.clear();
    recorder.audit.clear();
    tools = manager.ChatTools(true, recorder.Ports(true), {});
    Check(Find(tools, "mcp_fake_echo")->execute({{"text", "unconfirmed"}}, context).succeeded && recorder.asked.empty() &&
              recorder.audit.size() == 2,
        "A server with confirmation off still asked, or skipped its audit.");
    Check(Find(tools, "mcp_fake_change")->execute(Json::object(), context).succeeded, "The change tool failed.");
    std::this_thread::sleep_for(100ms);
    tools = manager.ChatTools(true, recorder.Ports(true), {});
    Check(Find(tools, "mcp_fake_added") != nullptr, "A tool announced by list_changed was not offered on the next turn.");
    manager.Shutdown();
}

// A llama-server stand-in that asks for one MCP tool, then answers from its result.
void TestToolLoopRunsAnMcpTool()
{
    httplib::Server backend;
    std::mutex mutex;
    std::vector<Json> requests;
    backend.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
    backend.Get("/v1/models", [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"mcp-fixture"}]})", "application/json"); });
    backend.Post("/v1/chat/completions",
        [&](const auto& request, auto& response)
        {
            std::lock_guard lock(mutex);
            requests.push_back(Json::parse(request.body));
            Json event;
            if (requests.size() == 1)
            {
                event = {{"choices", Json::array({{{"delta", {{"tool_calls", Json::array({{{"index", 0}, {"id", "call-1"}, {"type", "function"},
                                                                                    {"function", {{"name", "mcp_fake_echo"}, {"arguments", R"({"text":"from the model"})"}}}}})}}},
                                                     {"finish_reason", "tool_calls"}}})}};
            }
            else
            {
                event = {{"choices", Json::array({{{"delta", {{"content", "The server said it back."}}}, {"finish_reason", "stop"}}})}};
            }
            response.set_content("data: " + event.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
        });
    backend.new_task_queue = [] { return new httplib::ThreadPool(1); };
    const int port = backend.bind_to_any_port("127.0.0.1");
    std::jthread worker([&] { backend.listen_after_bind(); });
    while (!backend.is_running())
        std::this_thread::sleep_for(1ms);
    struct Stop
    {
        httplib::Server& server;
        ~Stop() { server.stop(); }
    } stopBackend{backend};

    messageRouter router;
    llmSettings llm;
    llm.host = "127.0.0.1";
    llm.port = port;
    llm.modelName = "mcp-fixture";
    // Tools are offered only with room for them, as on a real 32K server.
    llm.contextSize = 32768;
    llm.maxTokens = 512;
    llm.bAutoMaxTokens = llm.bAutoStartServer = llm.bVisionEnabled = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = embeddings.bAutoStartServer = false;
    aiProfile profile;
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(llm, embeddings, profile);
    Check(router.IsLLMAvailable(), "The MCP tool-loop fixture failed its model health admission.");

    mcpServerSettings server = FixtureServer();
    server.allowedTools = {"echo"};
    server.bRequireConfirmation = false;
    mcpSettings settings;
    settings.servers = {server};
    mcp::McpManager manager;
    manager.Configure(settings);
    GateRecorder recorder;
    agents::ToolRegistry registry;
    for (auto& tool : manager.ChatTools(true, recorder.Ports(false), {}))
        registry.Add(std::move(tool));
    Check(registry.Find("mcp_fake_echo") != nullptr, "The MCP tool was not offered to the loop: " + manager.StatusText());
    agents::ToolLoop loop(std::move(registry));
    std::vector<agents::ToolAuditRecord> records;
    loop.SetAuditHandler([&](const agents::ToolAuditRecord& record) { records.push_back(record); });
    const auto output = loop.Run(router, "Ask the echo tool.", {{"user", "Ask the echo tool."}}, {}, {}, {}, llm::PrivateMemoryAccess::Denied);
    Check(output.bSuccess && output.response == "The server said it back." && output.toolCallsExecuted == 1,
        "The tool loop did not run the MCP tool and answer: " + output.response + " " + output.reason);
    std::lock_guard lock(mutex);
    Check(requests.size() == 2, "The loop did not send the tool result back to the model.");
    const std::string second = requests[1].dump();
    Check(Contains(second, "mcp_fake_echo") && Contains(second, "echo: from the model") && Contains(second, "Untrusted output"),
        "The MCP result did not reach the model as labelled untrusted data.");
    Check(records.size() == 1 && records[0].executed && records[0].succeeded && recorder.audit.size() == 2,
        "The MCP call was missing from the loop's or the gate's audit.");
}

// Streamable HTTP, carried by a stand-in for WinHTTP over a loopback httplib server.
class LoopbackHttp final : public llm::cloud::CloudTransport
{
  public:
    llm::cloud::CloudHttpResult Post(const llm::cloud::CloudHttpRequest& request, const ChunkHandler& onChunk, std::stop_token) override
    {
        httplib::Client client(request.endpoint.host, request.endpoint.port);
        httplib::Headers headers;
        for (const auto& [name, value] : request.headers)
        {
            if (name != "Content-Type")
                headers.emplace(name, value);
        }
        const auto response = client.Post(request.path, headers, request.body, "application/json");
        llm::cloud::CloudHttpResult result;
        if (!response)
        {
            result.error = "unreachable";
            return result;
        }
        result.bReceived = true;
        result.status = response->status;
        for (const auto& name : request.responseHeaderNames)
        {
            if (response->has_header(name))
                result.responseHeaders.emplace_back(name, response->get_header_value(name));
        }
        // Small pieces, so events split across chunks are decoded too.
        for (std::size_t offset = 0; offset < response->body.size(); offset += 7)
        {
            if (!onChunk(response->status, std::string_view(response->body).substr(offset, 7)))
                break;
        }
        return result;
    }
};

void TestStreamableHttp()
{
    httplib::Server server;
    std::mutex mutex;
    std::vector<std::pair<Json, httplib::Headers>> seen;
    server.Post("/mcp",
        [&](const httplib::Request& request, httplib::Response& response)
        {
            const Json message = Json::parse(request.body);
            {
                std::lock_guard lock(mutex);
                seen.emplace_back(message, request.headers);
            }
            const std::string method = message.value("method", std::string{});
            if (!message.contains("id"))
            {
                response.status = 202;
                return;
            }
            if (method == "initialize")
            {
                response.set_header("Mcp-Session-Id", "session-123");
                response.set_content(Json{{"jsonrpc", "2.0"}, {"id", message["id"]},
                                         {"result", {{"protocolVersion", "2025-06-18"}, {"capabilities", {{"tools", Json::object()}}},
                                                        {"serverInfo", {{"name", "http-fixture"}}}}}}
                                         .dump(),
                    "application/json");
                return;
            }
            if (request.get_header_value("Mcp-Session-Id") != "session-123")
            {
                response.status = 400;
                return;
            }
            if (method == "tools/list")
            {
                response.set_content(Json{{"jsonrpc", "2.0"}, {"id", message["id"]},
                                         {"result", {{"tools", Json::array({{{"name", "echo"}, {"inputSchema", {{"type", "object"}}}}})}}}}
                                         .dump(),
                    "application/json");
                return;
            }
            // A tool answer as an event stream, with a notification first.
            const Json notice = {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}};
            const Json answer = {{"jsonrpc", "2.0"}, {"id", message["id"]},
                {"result", {{"content", Json::array({{{"type", "text"}, {"text", "over http"}}})}}}};
            response.set_content("event: message\ndata: " + notice.dump() + "\n\nevent: message\r\ndata: " + answer.dump() + "\r\n\r\n",
                "text/event-stream");
        });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::jthread worker([&] { server.listen_after_bind(); });
    while (!server.is_running())
        std::this_thread::sleep_for(1ms);
    struct Stop
    {
        httplib::Server& server;
        ~Stop() { server.stop(); }
    } stopServer{server};

    mcpServerSettings settings;
    settings.id = "web";
    settings.transport = "http";
    settings.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    settings.timeoutSeconds = 5;
    Check(ValidateMcpServerSettings(settings).empty(), "The loopback http server settings were refused.");
    mcp::McpClient client(mcp::MakeHttpTransport(settings, std::make_shared<LoopbackHttp>(), mcp::ProtocolVersion), 5000ms);
    std::string error;
    Check(client.Initialize({}, error) && client.ServerName() == "http-fixture", "The http handshake failed: " + error);
    std::vector<mcp::McpToolInfo> tools;
    Check(client.ListTools(tools, {}, error) && tools.size() == 1, "The http tool list failed: " + error);
    const auto call = client.CallTool("echo", Json::object(), {});
    Check(call.answered && call.text == "over http", "An event-stream answer was not decoded: " + call.text + call.error);
    client.Poll();
    Check(client.ToolsChanged(), "A notification inside the event stream was lost.");
    std::lock_guard lock(mutex);
    Check(seen.size() == 4, "Unexpected number of http requests: " + std::to_string(seen.size()));
    const auto header = [](const httplib::Headers& headers, const std::string& name)
    {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string{} : found->second;
    };
    Check(header(seen[0].second, "MCP-Protocol-Version").empty() && header(seen[2].second, "MCP-Protocol-Version") == "2025-06-18" &&
              header(seen[1].second, "Mcp-Session-Id") == "session-123" &&
              Contains(header(seen[3].second, "Accept"), "text/event-stream"),
        "The http requests did not carry the session and protocol headers.");
}
} // namespace

void RunMcpClientTests()
{
    TestSettingsAreValidatedAndOffByDefault();
    TestSchemasAreClosedOrRefused();
    TestServerThatExitsAtOnceIsDiagnosed();
    Check(std::filesystem::exists(FixturePath()), "The MCP server fixture was not found at " + actions::PathToUtf8(FixturePath()) +
                                                      "; build ReviaMcpServerFixture or set REVIA_MCP_SERVER_FIXTURE.");
    TestHandshakeListAndCall();
    TestTimeoutCancellationAndListChanged();
    TestManagerGatesEveryCall();
    TestToolLoopRunsAnMcpTool();
    TestStreamableHttp();
}
