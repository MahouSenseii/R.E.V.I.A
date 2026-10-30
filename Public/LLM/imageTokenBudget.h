#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace revia::llm
{
[[nodiscard]] inline std::size_t AttachmentImageTokenAllowance(
    const std::uint32_t imageWidth, const std::uint32_t imageHeight, const int minimumTokens)
{
    const std::size_t width = std::max<std::uint32_t>(imageWidth, 28);
    const std::size_t height = std::max<std::uint32_t>(imageHeight, 28);
    const std::size_t existing = ((width + 27) / 28) * ((height + 27) / 28) + 16;
    const auto minimum = static_cast<std::size_t>(std::clamp(minimumTokens, 0, 4096));
    if (minimum == 0)
        return std::min<std::size_t>(existing, 4096);
    // Backend patch grids round both dimensions; the configured floor is not an exact cost.
    const long double aspect = static_cast<long double>(std::max(imageWidth, 1u)) / std::max(imageHeight, 1u);
    const auto columns = static_cast<std::size_t>(std::ceil(std::sqrt(minimum * aspect)));
    const auto rows = static_cast<std::size_t>(std::ceil(std::sqrt(minimum / aspect)));
    return std::min<std::size_t>(std::max(existing, columns * rows + 16), 4096);
}
}
