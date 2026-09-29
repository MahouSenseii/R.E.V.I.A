#include "Skills/mcpToolExecutor.h"

#include "Skills/mcpManifest.h"

namespace revia::skills
{

McpToolExecutor::McpToolExecutor(std::shared_ptr<McpRegistry> inputRegistry)
    : registry(std::move(inputRegistry))
{
}

bool McpToolExecutor::Handles(const actions::ActionType type) const
{
    return type == actions::ActionType::McpTool;
}

bool McpToolExecutor::SplitToolReference(
    const std::string& value, std::string& outServer, std::string& outTool)
{
    const std::size_t slash = value.find('/');
    if (slash == std::string::npos) return false;
    outServer = value.substr(0, slash);
    outTool = value.substr(slash + 1);
    return ValidServerId(outServer) && ValidToolName(outTool);
}

actions::ActionResult McpToolExecutor::Execute(
    const actions::ActionRequest& request,
    const actions::PolicyDecision&)
{
    actions::ActionResult result;
    result.attempted = true;
    std::string server;
    std::string tool;
    if (!SplitToolReference(request.value, server, tool))
    {
        result.message = "An MCP tool is named as <server>/<tool>.";
        return result;
    }
    result.backend = "mcp:" + server;
    if (!registry)
    {
        result.message = "No MCP registry is loaded.";
        return result;
    }
    if (request.dryRun)
    {
        result.dryRun = true;
        result.succeeded = true;
        result.message = "Dry run: " + tool + " on " + server + " was not called.";
        return result;
    }
    const McpToolResult called = registry->Call(server, tool, request.arguments);
    if (!called.succeeded)
    {
        result.message = called.reason.empty() ? "The tool call failed." : called.reason;
        return result;
    }
    result.succeeded = !called.toolError;
    result.content = called.text;
    result.message = called.toolError
        ? "The tool " + tool + " on " + server + " reported an error: " +
            (called.text.empty() ? std::string("(no detail)") : called.text)
        : "Called " + tool + " on " + server + "." +
            (called.text.empty() ? std::string() : " It returned " + std::to_string(called.text.size()) + " characters.");
    return result;
}

} // namespace revia::skills
