#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace revia::agents
{
// Restores an exact source span, allowing layout changes only outside opaque C++ text.
// Matching a quotation establishes source identity, not the correctness of a finding.
[[nodiscard]] std::optional<std::string> ResolveSourceQuotation(std::string_view source, std::string_view quotation);
}
