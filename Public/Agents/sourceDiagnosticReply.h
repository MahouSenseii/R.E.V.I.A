#pragma once

#include "Agents/sourceDiagnostic.h"

namespace revia::agents
{
[[nodiscard]] std::string FormatSourceDiagnostic(const SourceDiagnostic& diagnostic, bool correctionOnly = false);
[[nodiscard]] bool IsSourceDiagnosticFollowup(
    std::string_view input, std::string_view previousAssistant, const SourceDiagnostic& diagnostic);
[[nodiscard]] bool WantsSourceDiagnosticCorrectionOnly(std::string_view input);
}
