#include "Audit/actionAuditLogger.h"

#include <chrono>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <limits>
#include <string_view>

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

std::string UtcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
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

std::string BoundedUtf8Prefix(const std::string& value, const std::size_t maximumBytes)
{
    if (value.size() <= maximumBytes) return value;
    std::size_t end = maximumBytes;
    while (end > 0 &&
        (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U)
    {
        --end;
    }
    return value.substr(0, end);
}

// Another writer holds the journal only while it finishes one append, so a brief
// retry distinguishes that from a journal that is genuinely unavailable. Without it,
// ordinary contention between two writers refuses an action outright.
constexpr int MaximumOpenAttempts = 5;
constexpr int OpenRetryMilliseconds = 20;

// Exclude competing writers, and repair -- never hide, complete or replay -- a partial
// previous record. A failed append must not permit dispatch.
//
// A torn trailing line used to refuse every later append for the life of the file, and
// because a refused intent append blocks dispatch, one crash mid-write disabled every
// future action until a human edited the journal. Failing closed was right; staying
// closed forever was not. The torn line is now terminated and followed by
// `recoveryRecord`, which names it incomplete and its outcome unknown. That is the same
// thing an orphan intent already means, said out loud: no byte of the torn record is
// altered, and nothing about it becomes safe to retry.
bool AppendDurably(const std::filesystem::path& path, const std::string& bytes,
    const std::string& recoveryRecord)
{
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < MaximumOpenAttempts; ++attempt)
    {
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) break;
        Sleep(OpenRetryMilliseconds);
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool valid = GetFileSizeEx(file, &size) != FALSE;
    bool torn = false;
    if (valid && size.QuadPart > 0)
    {
        LARGE_INTEGER last{};
        last.QuadPart = -1;
        char tail = 0;
        DWORD count = 0;
        valid = SetFilePointerEx(file, last, nullptr, FILE_END) &&
            ReadFile(file, &tail, 1, &count, nullptr) && count == 1;
        torn = valid && tail != '\n';
    }
    // One write, so a reader never sees the repair without the record that prompted it.
    const std::string payload = torn ? "\n" + recoveryRecord + bytes : bytes;
    LARGE_INTEGER end{};
    DWORD written = 0;
    valid = valid && payload.size() <= std::numeric_limits<DWORD>::max() &&
        SetFilePointerEx(file, end, nullptr, FILE_END) &&
        WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
        written == payload.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    return valid && closed;
#else
    const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (file < 0) return false;
    bool locked = false;
    for (int attempt = 0; attempt < MaximumOpenAttempts && !locked; ++attempt)
    {
        locked = flock(file, LOCK_EX | LOCK_NB) == 0;
        if (!locked) usleep(OpenRetryMilliseconds * 1000);
    }
    bool valid = locked;
    const off_t size = valid ? lseek(file, 0, SEEK_END) : -1;
    valid = valid && size >= 0;
    bool torn = false;
    if (valid && size > 0)
    {
        char tail = 0;
        valid = lseek(file, -1, SEEK_END) >= 0 && read(file, &tail, 1) == 1;
        torn = valid && tail != '\n';
    }
    const std::string payload = torn ? "\n" + recoveryRecord + bytes : bytes;
    valid = valid && lseek(file, 0, SEEK_END) >= 0;
    std::size_t offset = 0;
    while (valid && offset < payload.size())
    {
        const ssize_t count = write(file, payload.data() + offset, payload.size() - offset);
        if (count <= 0) valid = false;
        else offset += static_cast<std::size_t>(count);
    }
    valid = valid && fsync(file) == 0;
    const bool closed = close(file) == 0;
    return valid && closed;
#endif
}

} // namespace

ActionAuditLogger::ActionAuditLogger(std::filesystem::path inputPath)
    : path(std::move(inputPath))
{
}

bool ActionAuditLogger::Record(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision,
    const actions::ActionResult& result,
    const double elapsedMilliseconds,
    const std::string& transactionId)
{
    return WriteRecord(request, decision, result, elapsedMilliseconds, "result", transactionId);
}

bool ActionAuditLogger::RecordIntent(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision,
    const std::string& transactionId)
{
    actions::ActionResult pending;
    pending.dryRun = request.dryRun;
    pending.message = "Dispatch intended; completion is not yet recorded.";
    return WriteRecord(request, decision, pending, -1.0, "intent", transactionId);
}

bool ActionAuditLogger::WriteRecord(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision,
    const actions::ActionResult& result,
    const double elapsedMilliseconds,
    const char* recordType,
    const std::string& transactionId)
{
    std::lock_guard lock(mutex);
    try
    {
        std::error_code error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return false;
            }
        }

        nlohmann::json entry = {
            {"record_type", recordType},
            {"audit_transaction", transactionId},
            {"timestamp", UtcTimestamp()},
            {"action_id", request.id},
            {"requested_by", request.requestedBy},
            {"action", actions::ToString(request.type)},
            {"source", actions::PathToUtf8(request.source)},
            {"destination", actions::PathToUtf8(request.destination)},
            {"application", request.application},
            {"window_title", request.windowTitle},
            {"control", request.control},
            {"value_length", request.value.size()},
            {"canonical_source", actions::PathToUtf8(decision.canonicalSource)},
            {"canonical_destination", actions::PathToUtf8(decision.canonicalDestination)},
            {"dry_run", request.dryRun},
            {"risk", actions::ToString(decision.risk)},
            {"policy_verdict", actions::ToString(decision.verdict)},
            {"policy_reason", decision.reason},
            {"attempted", result.attempted},
            {"succeeded", result.succeeded},
            {"result", result.message},
            {"backend", result.backend}
        };
        if (std::string_view(recordType) == "intent")
        {
            // An orphan intent after a crash means unknown execution, not a
            // negative result that would make an automatic retry safe.
            entry.erase("attempted");
            entry.erase("succeeded");
        }
        if (elapsedMilliseconds >= 0.0)
        {
            entry["elapsed_ms"] = elapsedMilliseconds;
        }
        if (request.type == actions::ActionType::WebSearch)
        {
            constexpr std::size_t MaximumQueryBytes = 1024;
            constexpr std::size_t MaximumAuditedUrls = 10;
            constexpr std::size_t MaximumUrlBytes = 2048;
            nlohmann::json visitedUrls = nlohmann::json::array();
            for (std::size_t index = 0;
                 index < std::min(result.entries.size(), MaximumAuditedUrls);
                 ++index)
            {
                visitedUrls.push_back(BoundedUtf8Prefix(
                    result.entries[index], MaximumUrlBytes));
            }
            entry["internet_activity"] = {
                {"query", BoundedUtf8Prefix(request.value, MaximumQueryBytes)},
                {"query_truncated", request.value.size() > MaximumQueryBytes},
                {"backend", result.backend},
                {"visited_urls", std::move(visitedUrls)},
                {"source_count", result.entries.size()},
                {"grounding_bytes", result.content.size()}
            };
        }
        if (actions::IsDesktopControlAction(request.type))
        {
            // Key chords and pointer geometry are recorded in full because they are
            // what was done. Typed text is not: value_length above is deliberately the
            // only trace of it, so an audit trail cannot become a keystroke log.
            nlohmann::json desktop = {
                {"button", request.input.button ==
                    actions::ActionRequest::DesktopInput::PointerButton::Right ? "right"
                    : request.input.button ==
                        actions::ActionRequest::DesktopInput::PointerButton::Middle
                        ? "middle" : "left"},
                {"click_count", request.input.clickCount},
                {"scroll_clicks", request.input.scrollClicks},
                {"horizontal_scroll", request.input.horizontalScroll},
                {"keys", request.input.keys},
                {"targeted_point", request.input.hasPoint}
            };
            if (request.input.hasPoint)
            {
                desktop["requested_x"] = request.input.x;
                desktop["requested_y"] = request.input.y;
            }
            entry["desktop_input"] = std::move(desktop);
        }
        // Which route actually executed. Recorded for every targeted action rather
        // than only the resolved ones, because "this was aimed by pixels" and "this was
        // aimed at a control Windows named" are the two facts an audit most needs to
        // tell apart afterwards.
        if (request.resolution.kind != actions::TargetResolutionKind::None)
        {
            entry["target_resolution"] = actions::ToString(request.resolution.kind);
        }
        else if (request.input.hasPoint && actions::IsSynthesizedInputAction(request.type))
        {
            entry["target_resolution"] =
                actions::ToString(actions::TargetResolutionKind::RawCoordinate);
        }
        if (request.resolution.IsVisualRegionTarget())
        {
            // Bounded evidence for a target that had no element behind it: what it was
            // said to be, where, how sure, and which observation it came from. The
            // description is the model's own words about a button, which is not
            // sensitive in the way typed text is -- and typed text still never lands
            // here, only its length.
            entry["visual_target"] = {
                {"target_description", request.resolution.modelTarget},
                {"region", {
                    {"left", request.resolution.regionLeft},
                    {"top", request.resolution.regionTop},
                    {"right", request.resolution.regionRight},
                    {"bottom", request.resolution.regionBottom}}},
                {"model_confidence", request.resolution.modelConfidence},
                {"observation_id", request.resolution.observationId},
                {"observation_generation", request.resolution.observationGeneration},
                {"screen_digest", request.resolution.screenDigest},
                {"application", request.resolution.observedApplication},
                {"window_title", request.windowTitle},
                {"window", {
                    {"left", request.resolution.observedWindowLeft},
                    {"top", request.resolution.observedWindowTop},
                    {"right", request.resolution.observedWindowRight},
                    {"bottom", request.resolution.observedWindowBottom}}},
                // Whether the stronger route was tried, and what it said. Without this a
                // reader cannot tell an interface that exposes nothing from a resolver
                // that was never asked.
                {"uia_attempted", request.resolution.uiaAttempted},
                {"uia_failure", request.resolution.uiaFailure}
            };
        }
        if (request.resolution.IsUiaElementTarget())
        {
            entry["vision_resolution"] = {
                {"model_target", request.resolution.modelTarget},
                {"model_region", {
                    {"left", request.resolution.regionLeft},
                    {"top", request.resolution.regionTop},
                    {"right", request.resolution.regionRight},
                    {"bottom", request.resolution.regionBottom}}},
                {"model_confidence", request.resolution.modelConfidence},
                {"resolved_name", request.resolution.resolvedName},
                {"resolved_automation_id", request.resolution.resolvedAutomationId},
                {"resolved_runtime_id", request.resolution.resolvedRuntimeId},
                {"resolved_control_type", request.resolution.resolvedControlType},
                {"resolved_bounds", {
                    {"left", request.resolution.boundsLeft},
                    {"top", request.resolution.boundsTop},
                    {"right", request.resolution.boundsRight},
                    {"bottom", request.resolution.boundsBottom}}},
                {"spatial_agreement", request.resolution.spatialAgreement},
                {"name_agreement", request.resolution.nameAgreement},
                {"match_confidence", request.resolution.matchConfidence}
            };
        }
        // Written only if the previous append was interrupted. It marks the torn record
        // unknown rather than failed: an interrupted write says nothing about whether
        // the executor ran, which is precisely why it must not be replayed.
        const nlohmann::json recovery = {
            {"record_type", "recovery"},
            {"timestamp", UtcTimestamp()},
            {"observed_by_transaction", transactionId},
            {"result", "The preceding record was written incompletely. Its outcome is "
                "unknown: it was terminated, not completed, and must not be replayed."}
        };
        return AppendDurably(path, entry.dump() + '\n', recovery.dump() + '\n');
    }
    catch (const std::exception&)
    {
        return false;
    }
}

const std::filesystem::path& ActionAuditLogger::Path() const
{
    return path;
}

} // namespace revia::audit
