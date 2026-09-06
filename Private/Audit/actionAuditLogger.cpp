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

// Exclude competing writers and reject a partial previous record. A failed append
// must not permit dispatch or make the next append hide an incomplete JSON line.
bool AppendDurably(const std::filesystem::path& path, const std::string& bytes)
{
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool valid = GetFileSizeEx(file, &size) != FALSE;
    if (valid && size.QuadPart > 0)
    {
        LARGE_INTEGER last{};
        last.QuadPart = -1;
        char tail = 0;
        DWORD count = 0;
        valid = SetFilePointerEx(file, last, nullptr, FILE_END) &&
            ReadFile(file, &tail, 1, &count, nullptr) && count == 1 && tail == '\n';
    }
    LARGE_INTEGER end{};
    DWORD written = 0;
    valid = valid && bytes.size() <= std::numeric_limits<DWORD>::max() &&
        SetFilePointerEx(file, end, nullptr, FILE_END) &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    return valid && closed;
#else
    const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (file < 0) return false;
    bool valid = flock(file, LOCK_EX | LOCK_NB) == 0;
    const off_t size = valid ? lseek(file, 0, SEEK_END) : -1;
    valid = valid && size >= 0;
    if (valid && size > 0)
    {
        char tail = 0;
        valid = lseek(file, -1, SEEK_END) >= 0 && read(file, &tail, 1) == 1 && tail == '\n';
    }
    valid = valid && lseek(file, 0, SEEK_END) >= 0;
    std::size_t offset = 0;
    while (valid && offset < bytes.size())
    {
        const ssize_t count = write(file, bytes.data() + offset, bytes.size() - offset);
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
        if (request.resolution.visionResolved)
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
        return AppendDurably(path, entry.dump() + '\n');
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
