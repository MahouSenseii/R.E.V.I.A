#pragma once

#include "Agents/sourceDiagnostic.h"
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

class messageRouter;

namespace revia::agents
{
struct SourceReview
{
    bool succeeded = false;
    bool hasFinding = false;
    std::string finding;
    std::string reason;
    std::optional<SourceDiagnostic> diagnostic;
    std::vector<SourceDiagnostic> diagnostics;
};

// The caller retains source authorization and revalidates admission before delivery.
[[nodiscard]] SourceReview ReviewSavedSource(messageRouter& router, const std::string& evidence, std::stop_token stopToken);
[[nodiscard]] SourceReview ParseSourceReview(const std::string& response, const std::string& evidence);
[[nodiscard]] SourceReview ReviewNativeSavedSource(const std::string& evidence, std::stop_token stopToken);
}
