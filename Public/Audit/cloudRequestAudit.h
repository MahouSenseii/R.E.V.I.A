#pragma once

#include <cstddef>
#include <string>

namespace revia::audit
{

class EvidenceJournal;

// One cloud model request as the action audit records it: where it went and how big it
// was, never what it said. The digest identifies the exact request body, so a later
// question about one request can be answered by recomputing it from a copy, without the
// audit ever holding the conversation.
struct CloudRequestAuditEntry
{
    // "intent" before the bytes leave, "result" after the reply ends.
    std::string phase = "intent";
    std::string transactionId;
    std::string provider;
    std::string host;
    std::string model;
    std::size_t requestBytes = 0;
    std::size_t responseBytes = 0;
    // SHA-256 of the request body.
    std::string requestDigest;
    bool bExplicit = false;
    bool succeeded = false;
    int status = 0;
    double elapsedMilliseconds = -1.0;
    // A refusal or transport error in Revia's words; never response text.
    std::string outcome;
};

// The JSON line written to the audit, exposed so a test can check what is and is not in it.
[[nodiscard]] std::string CloudRequestAuditRecord(const CloudRequestAuditEntry& entry);

[[nodiscard]] bool AppendCloudRequestAudit(EvidenceJournal& journal, const CloudRequestAuditEntry& entry);

} // namespace revia::audit
