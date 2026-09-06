#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
#include <mutex>

namespace revia::audit
{

class ActionAuditLogger
{
public:
    explicit ActionAuditLogger(std::filesystem::path path);

    [[nodiscard]] bool RecordIntent(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision,
        const std::string& transactionId);

    [[nodiscard]] bool Record(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision,
        const actions::ActionResult& result,
        double elapsedMilliseconds = -1.0,
        const std::string& transactionId = {});

    [[nodiscard]] const std::filesystem::path& Path() const;

private:
    [[nodiscard]] bool WriteRecord(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision,
        const actions::ActionResult& result,
        double elapsedMilliseconds,
        const char* recordType,
        const std::string& transactionId);

    std::filesystem::path path;
    std::mutex mutex;
};

} // namespace revia::audit
