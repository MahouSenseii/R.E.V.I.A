#include "Audit/actionAuditLogger.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

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

} // namespace

ActionAuditLogger::ActionAuditLogger(std::filesystem::path inputPath)
    : path(std::move(inputPath))
{
}

bool ActionAuditLogger::Record(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision,
    const actions::ActionResult& result)
{
    std::lock_guard lock(mutex);
    std::error_code error;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return false;
        }
    }

    std::ofstream file(path, std::ios::app);
    if (!file.is_open())
    {
        return false;
    }

    nlohmann::json entry = {
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
        {"result", result.message}
    };
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
    file << entry.dump() << '\n';
    return file.good();
}

const std::filesystem::path& ActionAuditLogger::Path() const
{
    return path;
}

} // namespace revia::audit
