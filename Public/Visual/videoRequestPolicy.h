#pragma once

#include <string>
#include <string_view>

namespace revia::visual
{
[[nodiscard]] bool IsVideoGenerationRequest(std::string_view input);
[[nodiscard]] std::string VideoGenerationPrompt(std::string_view input);
}
