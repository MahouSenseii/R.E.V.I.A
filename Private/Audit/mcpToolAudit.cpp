#include "Audit/mcpToolAudit.h"
#include "Audit/evidenceJournal.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::audit
{

namespace
{
std::string UtcTimestamp()
{
    const std::time_t time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}
} // namespace

std::string McpToolAuditRecord(const McpToolAuditEntry& entry)
{
    nlohmann::json record = {{"phase", entry.phase}, {"audit_transaction", entry.transactionId}, {"timestamp", UtcTimestamp()},
        {"action", "mcp_tool_call"}, {"server", entry.server}, {"transport", entry.transport}, {"tool", entry.tool},
        {"arguments_digest", entry.argumentsDigest}, {"confirmed", entry.confirmed}};
    if (entry.phase != "intent")
    {
        record["succeeded"] = entry.succeeded;
        record["result_bytes"] = entry.resultBytes;
        record["outcome"] = entry.outcome;
        if (entry.elapsedMilliseconds >= 0.0)
            record["elapsed_ms"] = entry.elapsedMilliseconds;
    }
    return record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool AppendMcpToolAudit(EvidenceJournal& journal, const McpToolAuditEntry& entry)
{
    try
    {
        const std::string eventId = entry.transactionId.empty() ? NewJournalEventId() : entry.transactionId + ":" + entry.phase;
        const JournalKind kind = entry.phase == "intent" ? JournalKind::Intent : JournalKind::Result;
        return journal.AppendActionRecord(McpToolAuditRecord(entry), eventId, kind, entry.transactionId).Durable();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace revia::audit
