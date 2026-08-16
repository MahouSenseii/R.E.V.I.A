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

    [[nodiscard]] bool Record(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision,
        const actions::ActionResult& result);

    [[nodiscard]] const std::filesystem::path& Path() const;

private:
    std::filesystem::path path;
    std::mutex mutex;
};

} // namespace revia::audit
