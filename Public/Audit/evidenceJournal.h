#pragma once

#include "Core/evidenceRef.h"
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::audit
{
enum class JournalKind
{
    Intent,
    Observation,
    Result,
    Verification,
    Correction,
    Review
};
enum class AppendState
{
    Durable,
    Duplicate,
    Degraded,
    Rejected
};
enum class JournalIoBoundary
{
    BeforeWrite,
    BeforeFlush
};
using JournalIoGate = std::function<bool(JournalIoBoundary)>;

struct JournalEvent
{
    core::EvidenceRef evidence;
    JournalKind kind = JournalKind::Observation;
    std::string transactionId;
    std::string redactedSummary;
};

struct AppendReceipt
{
    AppendState state = AppendState::Degraded;
    std::string eventId;
    std::string error;
    [[nodiscard]] bool Durable() const
    {
        return state == AppendState::Durable || state == AppendState::Duplicate;
    }
};

struct EvidenceQuery
{
    runtime::RuntimeStamp stamp;
    memory::MemoryScope scope;
    std::string transactionId;
    std::vector<JournalKind> kinds;
};

struct JournalHealth
{
    bool degraded = false;
    std::string error;
    std::size_t quarantinedRecords = 0;
    std::size_t droppedTelemetry = 0;
    std::vector<std::string> unresolvedTransactions;
};

[[nodiscard]] std::string NewJournalEventId();

// This owner persists receipts only. Recovery never calls an executor or grants authority.
class EvidenceJournal
{
  public:
    explicit EvidenceJournal(std::filesystem::path path, std::size_t telemetryCapacity = 64, JournalIoGate ioGate = {});
    [[nodiscard]] AppendReceipt Append(const JournalEvent& event);
    [[nodiscard]] std::vector<core::EvidenceRef> Read(const EvidenceQuery& query);
    [[nodiscard]] JournalHealth Recover();
    [[nodiscard]] JournalHealth Health() const;
    [[nodiscard]] bool QueueTelemetry(const JournalEvent& event);
    [[nodiscard]] bool FlushTelemetry();
    [[nodiscard]] const std::filesystem::path& Path() const;

    // The adapter retains historical readable fields; all bytes still use this owner.
    [[nodiscard]] AppendReceipt AppendActionRecord(const std::string& record, const std::string& eventId, JournalKind kind,
        const std::string& transactionId, const std::optional<core::EvidenceRef>& evidence = {});

  private:
    [[nodiscard]] AppendReceipt AppendRecord(const std::string& record, const std::string& eventId, bool retainActionObservation = false);
    [[nodiscard]] AppendReceipt AppendLocked(const JournalEvent& event);
    std::filesystem::path path;
    std::size_t telemetryCapacity;
    JournalIoGate ioGate;
    mutable std::mutex mutex;
    std::deque<JournalEvent> telemetry;
    std::size_t pendingDrops = 0;
    std::optional<JournalEvent> droppedContext;
    JournalHealth health;
};
}
