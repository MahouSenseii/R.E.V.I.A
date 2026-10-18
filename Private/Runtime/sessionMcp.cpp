#include "Runtime/reviaSession.h"

#include "Audit/evidenceJournal.h"
#include "Audit/mcpToolAudit.h"

namespace revia::runtime
{

std::vector<agents::ChatTool> ReviaSession::McpChatTools(const ConfirmationHandler& handler, const std::stop_token stopToken)
{
    if (!mcpManager.AnyEnabled() || Audience().kind != identity::AudienceKind::Private)
        return {};
    mcp::McpGatePorts ports;
    // The same journal as every action, the intent written before a byte is sent. Without
    // it the manager offers no MCP tool at all.
    if (const std::shared_ptr<audit::EvidenceJournal> journal = actionRuntime.EvidenceJournalOwner())
        ports.audit = [journal](const audit::McpToolAuditEntry& entry) { return audit::AppendMcpToolAudit(*journal, entry); };
    if (handler)
    {
        ports.confirm = [this, handler](const mcp::McpConfirmationRequest& request)
        {
            // The window's confirmation shows an action request; this one names the server,
            // the tool and its arguments. Any yes counts once: an MCP call never earns a
            // standing approval, because Revia cannot know what the next one would do.
            actions::ActionRequest action;
            action.id = actions::NewActionId();
            action.type = actions::ActionType::Unknown;
            action.application = "MCP server \"" + request.server + "\"";
            action.windowTitle = request.tool;
            action.value = request.arguments;
            action.requestedBy = "mcp_tool";
            actions::PolicyDecision decision;
            decision.verdict = actions::PolicyVerdict::RequiresConfirmation;
            decision.risk = actions::RiskLevel::ReversibleWrite;
            decision.reason = "Revia wants to call the tool \"" + request.tool + "\" on your MCP server \"" + request.server +
                              "\" with these arguments: " + request.arguments +
                              "\nIt is an external program, so Revia cannot know what it will do.";
            SetState(RuntimeState::WaitingForConfirmation, decision.reason);
            const actions::ConfirmationChoice choice = handler(action, decision);
            SetState(RuntimeState::Thinking, "Continuing the reply.");
            return actions::Granted(choice);
        };
    }
    return mcpManager.ChatTools(true, ports, stopToken);
}

} // namespace revia::runtime
