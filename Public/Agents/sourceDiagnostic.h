#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace revia::agents
{
enum class SourceDiagnosticKind
{
    CheckedRangeBoundary,
    UncheckedRangeBoundary,
    LocalReferenceReturn
};

// Native deductions about the supplied C++ subset, never deserialized model claims.
struct SourceDiagnostic
{
    SourceDiagnosticKind kind = SourceDiagnosticKind::CheckedRangeBoundary;
    std::string function;
    std::string evidence;
    std::string condition;
    std::string consequence;
    std::string correction;
    std::string original;
    std::string replacement;
};

// Conservative checks; an empty result makes no claim that arbitrary code is correct.
[[nodiscard]] std::vector<SourceDiagnostic> AnalyzeSourceDiagnostics(std::string_view source);
}
