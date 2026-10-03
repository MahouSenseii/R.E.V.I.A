#pragma once

#include <string>

namespace revia::audit
{
// SHA256 of exact bytes, including embedded NULs. This identifies content; it does not authenticate its author.
[[nodiscard]] std::string ContentDigest(const std::string& bytes);
}
