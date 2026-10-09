#include "Audit/evidenceJournal.h"
#include "Audit/contentDigest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace revia::audit
{
namespace
{
using Json = nlohmann::json;
constexpr int MaximumOpenAttempts = 5;
constexpr int OpenRetryMilliseconds = 20;
constexpr std::size_t MaximumSummaryBytes = 4096;

const char* KindName(JournalKind kind)
{
    switch (kind)
    {
    case JournalKind::Intent:
        return "intent";
    case JournalKind::Observation:
        return "observation";
    case JournalKind::Result:
        return "result";
    case JournalKind::Verification:
        return "verification";
    case JournalKind::Correction:
        return "correction";
    case JournalKind::Review:
        return "review";
    }
    return "unknown";
}
std::string CanonicalIdentity(Json record)
{
    record.erase("timestamp");
    return record.dump();
}
struct Projection
{
    std::vector<Json> records;
    std::map<std::string, std::string> identities;
    JournalHealth health;
    bool conflictingIds = false;
    bool unsupportedVersion = false;
};
Projection Project(const std::string& bytes)
{
    Projection projection;
    std::set<std::string> unresolved;
    std::size_t start = 0;
    while (start < bytes.size())
    {
        const auto end = bytes.find('\n', start);
        if (end == std::string::npos)
        {
            ++projection.health.quarantinedRecords;
            break;
        }
        const auto line = bytes.substr(start, end - start);
        start = end + 1;
        auto record = Json::parse(line, nullptr, false);
        if (line.size() > core::MaximumContractJsonBytes || !record.is_object())
        {
            ++projection.health.quarantinedRecords;
            continue;
        }
        const auto nextEnd = bytes.find('\n', start);
        if (nextEnd != std::string::npos)
        {
            const auto next = Json::parse(bytes.substr(start, nextEnd - start), nullptr, false);
            if (next.is_object() && next.contains("record_type") && next["record_type"] == "recovery" &&
                next.contains("quarantined_digest") && next["quarantined_digest"] == ContentDigest(line))
            {
                ++projection.health.quarantinedRecords;
                continue;
            }
        }
        if ((record.contains("record_type") && !record["record_type"].is_string()) ||
            (record.contains("audit_transaction") && !record["audit_transaction"].is_string()))
        {
            ++projection.health.quarantinedRecords;
            continue;
        }
        if (record.contains("evidence"))
        {
            core::EvidenceRef reference;
            if (!core::DeserializeEvidenceRef(record["evidence"].dump(), reference))
            {
                ++projection.health.quarantinedRecords;
                continue;
            }
        }
        if (record.contains("journal_version"))
        {
            if (!record["journal_version"].is_number_integer() || record["journal_version"] != 1)
            {
                ++projection.health.quarantinedRecords;
                projection.unsupportedVersion = true;
                continue;
            }
            if (!record.contains("event_id") || !record["event_id"].is_string() || record["event_id"].get<std::string>().empty())
            {
                ++projection.health.quarantinedRecords;
                continue;
            }
            const auto id = record["event_id"].get<std::string>();
            const auto canonical = CanonicalIdentity(record);
            const auto [found, inserted] = projection.identities.emplace(id, canonical);
            if (!inserted)
            {
                if (found->second != canonical)
                {
                    ++projection.health.quarantinedRecords;
                    projection.conflictingIds = true;
                }
                continue;
            }
        }
        const auto kind = record.value("record_type", std::string());
        const auto transaction = record.value("audit_transaction", std::string());
        if (!transaction.empty())
        {
            if (kind == "intent")
                unresolved.insert(transaction);
            else if (kind == "result" || kind == "verification")
                unresolved.erase(transaction);
        }
        if (record.contains("dropped_count") && record["dropped_count"].is_number_unsigned())
            projection.health.droppedTelemetry += record["dropped_count"].get<std::size_t>();
        projection.records.push_back(std::move(record));
    }
    projection.health.unresolvedTransactions.assign(unresolved.begin(), unresolved.end());
    projection.health.degraded = projection.health.quarantinedRecords != 0;
    if (projection.conflictingIds)
        projection.health.error = "Conflicting event IDs quarantined; persistence refused.";
    else if (projection.unsupportedVersion)
        projection.health.error = "Unsupported journal version quarantined; persistence refused.";
    else if (projection.health.degraded)
        projection.health.error = "Incomplete or invalid journal records quarantined; outcomes remain unknown.";
    return projection;
}
std::string ReadBytes(const std::filesystem::path& path, const JournalInputFactory& factory)
{
    std::unique_ptr<std::istream> source;
    if (factory)
        source = factory(path);
    else
    {
        auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
        if (!file->is_open())
        {
            if (!std::filesystem::exists(path))
                return {};
            throw std::runtime_error("Journal cannot be read.");
        }
        source = std::move(file);
    }
    if (!source)
        throw std::runtime_error("Journal reader unavailable.");
    auto& input = *source;
    std::string bytes;
    char buffer[8192];
    for (;;)
    {
        input.read(buffer, sizeof(buffer));
        if (input.bad() || (input.fail() && !input.eof()))
            throw std::runtime_error("Journal read failed; history is incomplete.");
        bytes.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (input.eof())
            return bytes;
    }
}

// The builder runs under the OS writer lock, including duplicate admission.
bool AppendDurably(const std::filesystem::path& path, const std::function<std::string(const std::string&)>& build,
    const JournalIoGate& gate, std::string& error)
{
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < MaximumOpenAttempts; ++attempt)
    {
        file =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        const auto failure = GetLastError();
        if (failure != ERROR_SHARING_VIOLATION && failure != ERROR_LOCK_VIOLATION)
            break;
        Sleep(OpenRetryMilliseconds);
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "Journal writer unavailable (open/lock failed).";
        return false;
    }
    bool valid = true;
    std::string previous;
    char buffer[65536];
    DWORD count = 0;
    while (valid)
    {
        valid = ReadFile(file, buffer, sizeof(buffer), &count, nullptr) != FALSE;
        if (!valid || count == 0)
            break;
        previous.append(buffer, count);
    }
    std::string payload;
    try
    {
        if (valid)
            payload = build(previous);
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        valid = false;
    }
    if (valid && !payload.empty())
    {
        LARGE_INTEGER end{};
        valid = SetFilePointerEx(file, end, nullptr, FILE_END) != FALSE;
        try
        {
            valid = valid && (!gate || gate(JournalIoBoundary::BeforeWrite));
        }
        catch (...)
        {
            valid = false;
        }
        DWORD written = 0;
        const std::size_t bodySize = payload.size() - 1;
        valid = valid && bodySize <= std::numeric_limits<DWORD>::max() &&
                WriteFile(file, payload.data(), static_cast<DWORD>(bodySize), &written, nullptr) && written == bodySize;
        try
        {
            valid = valid && (!gate || gate(JournalIoBoundary::BeforeFlush));
        }
        catch (...)
        {
            valid = false;
        }
        valid = valid && FlushFileBuffers(file);
        if (valid)
        {
            valid = WriteFile(file, "\n", 1, &written, nullptr) && written == 1;
            if (valid && !FlushFileBuffers(file))
            {
                LARGE_INTEGER last{};
                last.QuadPart = -1;
                (void)SetFilePointerEx(file, last, nullptr, FILE_END);
                (void)SetEndOfFile(file);
                valid = false;
            }
        }
    }
    const bool closed = CloseHandle(file) != FALSE;
#else
    const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (file < 0)
    {
        error = "Journal writer unavailable (open failed).";
        return false;
    }
    bool valid = false;
    for (int attempt = 0; attempt < MaximumOpenAttempts && !valid; ++attempt)
    {
        valid = flock(file, LOCK_EX | LOCK_NB) == 0;
        if (!valid)
            usleep(OpenRetryMilliseconds * 1000);
    }
    std::string previous;
    char buffer[65536];
    while (valid)
    {
        const auto count = read(file, buffer, sizeof(buffer));
        if (count < 0)
        {
            valid = false;
            break;
        }
        if (count == 0)
            break;
        previous.append(buffer, static_cast<std::size_t>(count));
    }
    std::string payload;
    try
    {
        if (valid)
            payload = build(previous);
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        valid = false;
    }
    if (valid && !payload.empty())
    {
        valid = lseek(file, 0, SEEK_END) >= 0;
        try
        {
            valid = valid && (!gate || gate(JournalIoBoundary::BeforeWrite));
        }
        catch (...)
        {
            valid = false;
        }
        std::size_t offset = 0;
        while (valid && offset < payload.size() - 1)
        {
            const auto count = write(file, payload.data() + offset, payload.size() - 1 - offset);
            if (count <= 0)
                valid = false;
            else
                offset += static_cast<std::size_t>(count);
        }
        try
        {
            valid = valid && (!gate || gate(JournalIoBoundary::BeforeFlush));
        }
        catch (...)
        {
            valid = false;
        }
        valid = valid && fsync(file) == 0;
        if (valid)
        {
            valid = write(file, "\n", 1) == 1;
            if (valid && fsync(file) != 0)
            {
                const auto end = lseek(file, 0, SEEK_END);
                if (end > 0)
                    (void)ftruncate(file, end - 1);
                valid = false;
            }
        }
    }
    const bool closed = close(file) == 0;
#endif
    if ((!valid || !closed) && error.empty())
        error = "Journal write/flush/close failed; outcome is uncertain.";
    return valid && closed;
}
Json Encode(const JournalEvent& event)
{
    std::string serialized;
    const auto validation = core::SerializeEvidenceRef(event.evidence, serialized);
    if (!validation)
        throw std::invalid_argument(validation.code + ": " + validation.message);
    return {{"journal_version", 1}, {"event_id", event.evidence.id}, {"record_type", KindName(event.kind)},
        {"audit_transaction", event.transactionId}, {"evidence", Json::parse(serialized)}, {"summary", event.redactedSummary}};
}
}

std::string NewJournalEventId()
{
    static std::atomic<std::uint64_t> sequence{0};
    const auto time = std::chrono::system_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    return std::to_string(time) + "-" + std::to_string(process) + "-" + std::to_string(++sequence);
}
EvidenceJournal::EvidenceJournal(std::filesystem::path inputPath, std::size_t capacity, JournalIoGate gate, JournalInputFactory factory)
    : path(std::move(inputPath)), telemetryCapacity(capacity), ioGate(std::move(gate)), inputFactory(std::move(factory))
{
}
AppendReceipt EvidenceJournal::AppendRecord(const std::string& record, const std::string& eventId, bool retainActionObservation)
{
    if (record.size() > core::MaximumContractJsonBytes)
        return {AppendState::Rejected, eventId, "Journal record exceeds the bounded record limit."};
    AppendReceipt receipt{AppendState::Degraded, eventId, {}};
    try
    {
        std::error_code failure;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), failure);
        if (failure)
            throw std::runtime_error("Journal directory unavailable: " + failure.message());
        auto entry = Json::parse(record);
        auto canonical = CanonicalIdentity(entry);
        const bool stored = AppendDurably(
            path,
            [&](const std::string& previous)
            {
                auto projection = Project(previous);
                const auto dropped = health.droppedTelemetry;
                health = projection.health;
                health.droppedTelemetry = std::max(dropped, health.droppedTelemetry);
                if (projection.conflictingIds || projection.unsupportedVersion)
                {
                    receipt.state = AppendState::Rejected;
                    receipt.error = health.error;
                    return std::string();
                }
                const auto found = projection.identities.find(eventId);
                if (found != projection.identities.end())
                {
                    if (retainActionObservation && entry.contains("evidence"))
                    {
                        const auto original = Json::parse(found->second);
                        if (original.contains("evidence"))
                        {
                            // Adapter retries refer to the original operation and retain its observation time.
                            entry["evidence"]["observedAtUnixMs"] = original["evidence"]["observedAtUnixMs"];
                            canonical = CanonicalIdentity(entry);
                        }
                    }
                    if (found->second == canonical)
                        receipt.state = AppendState::Duplicate;
                    else
                    {
                        receipt.state = AppendState::Rejected;
                        receipt.error = "Event ID already names different content.";
                    }
                    return std::string();
                }
                std::string payload;
                if (!previous.empty() && previous.back() != '\n')
                {
                    const auto tailStart = previous.find_last_of('\n');
                    const auto tail = previous.substr(tailStart == std::string::npos ? 0 : tailStart + 1);
                    const Json recovery = {{"record_type", "recovery"}, {"quarantined_digest", ContentDigest(tail)},
                        {"observed_by_transaction", entry.value("audit_transaction", std::string())},
                        {"result", "The preceding record was written incompletely. Its outcome is unknown: it was terminated, not "
                                   "completed, and must not be replayed."}};
                    // Invalidate complete JSON before its newline, including when repair itself is interrupted.
                    const bool completeJsonTail = Json::parse(tail, nullptr, false).is_object();
                    payload = (completeJsonTail ? "!\n" : "\n") + recovery.dump() + '\n';
                }
                payload += record + '\n';
                return payload;
            },
            ioGate, receipt.error);
        if (stored && receipt.state == AppendState::Degraded)
        {
            receipt.state = AppendState::Durable;
            const auto dropped = health.droppedTelemetry;
            health = Project(ReadBytes(path, inputFactory)).health;
            health.droppedTelemetry = std::max(dropped, health.droppedTelemetry);
        }
        else if (!stored)
            receipt.state = AppendState::Degraded;
    }
    catch (const std::exception& failure)
    {
        receipt.error = failure.what();
        health.degraded = true;
        health.error = receipt.error;
    }
    if (!receipt.Durable() && receipt.state == AppendState::Degraded)
    {
        health.degraded = true;
        health.error = receipt.error;
    }
    return receipt;
}
AppendReceipt EvidenceJournal::AppendLocked(const JournalEvent& event)
{
    if ((event.kind == JournalKind::Intent || event.kind == JournalKind::Result) && event.transactionId.empty())
        return {AppendState::Rejected, event.evidence.id, "Intent/result requires a transaction identity."};
    if (std::string(KindName(event.kind)) == "unknown" || event.redactedSummary.size() > MaximumSummaryBytes)
        return {AppendState::Rejected, event.evidence.id, "Invalid event kind or summary exceeds the bounded metadata limit."};
    try
    {
        const auto record = Encode(event).dump();
        if (record.size() > core::MaximumContractJsonBytes)
            return {AppendState::Rejected, event.evidence.id, "Journal record exceeds the bounded record limit."};
        return AppendRecord(record, event.evidence.id);
    }
    catch (const std::exception& failure)
    {
        return {AppendState::Rejected, event.evidence.id, failure.what()};
    }
}
AppendReceipt EvidenceJournal::Append(const JournalEvent& event)
{
    std::lock_guard lock(mutex);
    return AppendLocked(event);
}
AppendReceipt EvidenceJournal::AppendActionRecord(const std::string& record, const std::string& eventId, JournalKind kind,
    const std::string& transactionId, const std::optional<core::EvidenceRef>& evidence)
{
    std::lock_guard lock(mutex);
    try
    {
        auto entry = Json::parse(record);
        if (!entry.is_object() || eventId.empty())
            throw std::invalid_argument("Invalid action audit record/identity.");
        entry["journal_version"] = 1;
        entry["event_id"] = eventId;
        entry["record_type"] = KindName(kind);
        entry["audit_transaction"] = transactionId;
        if (evidence)
        {
            std::string serialized;
            const auto validation = core::SerializeEvidenceRef(*evidence, serialized);
            if (!validation || evidence->id != eventId)
                throw std::invalid_argument("Invalid captured action evidence.");
            entry["evidence"] = Json::parse(serialized);
        }
        return AppendRecord(entry.dump(), eventId, true);
    }
    catch (const std::exception& failure)
    {
        return {AppendState::Rejected, eventId, failure.what()};
    }
}
std::vector<core::EvidenceRef> EvidenceJournal::Read(const EvidenceQuery& query)
{
    std::lock_guard lock(mutex);
    std::vector<core::EvidenceRef> references;
    try
    {
        const auto projection = Project(ReadBytes(path, inputFactory));
        const auto dropped = health.droppedTelemetry;
        health = projection.health;
        health.droppedTelemetry = std::max(dropped, health.droppedTelemetry);
        for (const auto& record : projection.records)
        {
            if (!record.contains("evidence"))
                continue;
            if (!query.transactionId.empty() && record.value("audit_transaction", std::string()) != query.transactionId)
                continue;
            if (!query.kinds.empty() && std::none_of(query.kinds.begin(), query.kinds.end(),
                                            [&](JournalKind kind) { return record.value("record_type", std::string()) == KindName(kind); }))
                continue;
            core::EvidenceRef reference;
            if (!core::DeserializeEvidenceRef(record["evidence"].dump(), reference))
                continue;
            if (core::SameRuntimeStamp(reference.stamp, query.stamp) && core::SameMemoryScope(reference.scope, query.scope))
                references.push_back(std::move(reference));
        }
    }
    catch (const std::exception& failure)
    {
        health.degraded = true;
        health.error = failure.what();
    }
    return references;
}
bool EvidenceJournal::HasUnresolvedForTask(const runtime::RuntimeStamp& stamp, const memory::MemoryScope& scope)
{
    std::lock_guard lock(mutex);
    try
    {
        const auto projection = Project(ReadBytes(path, inputFactory));
        std::set<std::string> pending;
        if (projection.unsupportedVersion || projection.conflictingIds)
            return true;
        for (const auto& record : projection.records)
        {
            if (!record.contains("evidence"))
                continue;
            core::EvidenceRef reference;
            if (!core::DeserializeEvidenceRef(record["evidence"].dump(), reference))
                continue;
            auto captured = reference.stamp;
            captured.attemptId = stamp.attemptId;
            captured.policyVersion = stamp.policyVersion;
            if (!core::SameRuntimeStamp(captured, stamp) || !core::SameMemoryScope(reference.scope, scope))
                continue;
            const auto transaction = record.value("audit_transaction", std::string());
            const auto kind = record.value("record_type", std::string());
            if (transaction.empty())
                continue;
            if (kind == "intent")
                pending.insert(transaction);
            else if (kind == "result" || kind == "verification")
                pending.erase(transaction);
        }
        return !pending.empty();
    }
    catch (const std::exception& failure)
    {
        health.degraded = true;
        health.error = failure.what();
        return true;
    }
}

JournalHealth EvidenceJournal::Recover()
{
    std::lock_guard lock(mutex);
    try
    {
        const auto dropped = health.droppedTelemetry;
        health = Project(ReadBytes(path, inputFactory)).health;
        health.droppedTelemetry = std::max(dropped, health.droppedTelemetry);
    }
    catch (const std::exception& failure)
    {
        health.degraded = true;
        health.error = failure.what();
    }
    return SnapshotHealthLocked();
}
JournalHealth EvidenceJournal::SnapshotHealthLocked() const
{
    const std::lock_guard queueLock(telemetryMutex);
    auto snapshot = health;
    snapshot.durableDroppedTelemetry = health.droppedTelemetry;
    snapshot.pendingDroppedTelemetry = pendingDrops + flushingDrops;
    snapshot.droppedTelemetry = snapshot.durableDroppedTelemetry + snapshot.pendingDroppedTelemetry;
    return snapshot;
}
JournalHealth EvidenceJournal::Health() const
{
    std::lock_guard lock(mutex);
    return SnapshotHealthLocked();
}
bool EvidenceJournal::QueueTelemetry(const JournalEvent& event)
{
    if (event.kind == JournalKind::Intent || event.kind == JournalKind::Result || !core::ValidateEvidenceRef(event.evidence) ||
        event.redactedSummary.size() > MaximumSummaryBytes)
        return false;
    const std::lock_guard queueLock(telemetryMutex);
    if (telemetry.size() >= telemetryCapacity)
    {
        if (!droppedContext)
            droppedContext = event;
        ++pendingDrops;
        return false;
    }
    telemetry.push_back(event);
    return true;
}
bool EvidenceJournal::FlushTelemetry()
{
    const std::lock_guard flushLock(telemetryFlushMutex);
    std::deque<JournalEvent> batch;
    std::size_t batchDrops = 0;
    std::optional<JournalEvent> batchContext;
    {
        const std::lock_guard queueLock(telemetryMutex);
        batch.swap(telemetry);
        batchDrops = pendingDrops;
        batchContext = std::move(droppedContext);
        pendingDrops = 0;
        droppedContext.reset();
        flushingDrops = batchDrops;
    }
    const auto restore = [&]
    {
        const std::lock_guard queueLock(telemetryMutex);
        pendingDrops += flushingDrops;
        flushingDrops = 0;
        if (batchDrops != 0 && batchContext)
            droppedContext = std::move(batchContext);
        while (!telemetry.empty())
        {
            if (batch.size() < telemetryCapacity)
                batch.push_back(std::move(telemetry.front()));
            else
            {
                if (!droppedContext)
                    droppedContext = telemetry.front();
                ++pendingDrops;
            }
            telemetry.pop_front();
        }
        telemetry.swap(batch);
    };
    if (batchDrops != 0 && batchContext)
    {
        auto marker = *batchContext;
        marker.evidence.id = "telemetry-dropped-" + NewJournalEventId();
        marker.kind = JournalKind::Observation;
        marker.transactionId.clear();
        marker.redactedSummary = "Nonessential telemetry was dropped by the bounded queue.";
        auto record = Encode(marker);
        record["dropped_count"] = batchDrops;
        bool durable = false;
        {
            const std::lock_guard diskLock(mutex);
            durable = AppendRecord(record.dump(), marker.evidence.id).Durable();
            if (durable)
            {
                const std::lock_guard queueLock(telemetryMutex);
                flushingDrops = 0;
                batchDrops = 0;
            }
        }
        if (!durable)
        {
            restore();
            return false;
        }
    }
    while (!batch.empty())
    {
        bool durable = false;
        {
            const std::lock_guard diskLock(mutex);
            durable = AppendLocked(batch.front()).Durable();
        }
        if (!durable)
        {
            restore();
            return false;
        }
        batch.pop_front();
    }
    return true;
}
const std::filesystem::path& EvidenceJournal::Path() const
{
    return path;
}
}
