#pragma once

#include <cstddef>
#include <string>

namespace revia::audit
{

class EvidenceJournal;

// One MCP tool call as the action audit records it: which server and tool, a digest of
// the arguments, and how much came back, never the arguments or the result themselves.
struct McpToolAuditEntry
{
    // "intent" before the request leaves, "result" after it ends, "refused" when the
    // call was stopped before anything was sent.
    std::string phase = "intent";
    std::string transactionId;
    std::string server;
    std::string transport;
    std::string tool;
    // SHA-256 of the exact arguments sent.
    std::string argumentsDigest;
    bool confirmed = false;
    bool succeeded = false;
    std::size_t resultBytes = 0;
    double elapsedMilliseconds = -1.0;
    // A refusal or error in Revia's words; never result text.
    std::string outcome;
};

// The JSON line written to the audit, exposed so a test can check what is and is not in it.
[[nodiscard]] std::string McpToolAuditRecord(const McpToolAuditEntry& entry);

[[nodiscard]] bool AppendMcpToolAudit(EvidenceJournal& journal, const McpToolAuditEntry& entry);

} // namespace revia::audit
