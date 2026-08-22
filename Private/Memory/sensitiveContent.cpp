#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace revia::memory
{

bool ContainsSensitiveContent(const std::string& text)
{
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

    constexpr std::string_view Markers[] = {
        "password", "passcode", "api key", "secret key", "access token",
        "private key", "credit card", "social security", "recovery code"
    };
    return std::any_of(std::begin(Markers), std::end(Markers),
        [&lowered](const std::string_view marker)
        {
            return lowered.find(marker) != std::string::npos;
        });
}

} // namespace revia::memory
