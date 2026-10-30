#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revia::attachments
{

// Empty means this bounded metadata policy does not answer the request.
[[nodiscard]] std::string VideoSamplingLimitation(const std::string& input, const std::vector<std::int64_t>& timestampsMs);

}
