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
    file << entry.dump() << '\n';
    return file.good();
}

const std::filesystem::path& ActionAuditLogger::Path() const
{
    return path;
}

} // namespace revia::audit
