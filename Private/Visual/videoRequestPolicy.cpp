#include "Visual/videoRequestPolicy.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace revia::visual
{
namespace
{
std::string Trim(const std::string_view input)
{
    const auto first = input.find_first_not_of(" \t\r\n");
    return first == std::string_view::npos ? std::string{}
                                           : std::string(input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1));
}

bool Word(const std::string& input, const std::string_view word)
{
    for (auto at = input.find(word); at != std::string::npos; at = input.find(word, at + 1))
        if ((at == 0 || !std::isalnum(static_cast<unsigned char>(input[at - 1]))) &&
            (at + word.size() == input.size() || !std::isalnum(static_cast<unsigned char>(input[at + word.size()]))))
            return true;
    return false;
}
}

std::string VideoGenerationPrompt(const std::string_view input)
{
    const auto value = Trim(input);
    if (value.starts_with("/video "))
        return Trim(std::string_view(value).substr(7));
    if (value.starts_with("/animate "))
        return Trim(std::string_view(value).substr(9));
    return value;
}

bool IsVideoGenerationRequest(const std::string_view input)
{
    if (input.empty() || input.size() > 8192)
        return false;
    std::string value = Trim(input);
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "/video status" || value == "/video cancel" || value == "/video")
        return false;
    if (value.starts_with("/video ") || value.starts_with("/animate "))
        return true;
    for (const auto prefix : {"please ", "can you ", "could you ", "would you "})
        if (value.starts_with(prefix))
            value.erase(0, std::string_view(prefix).size());
    if (value.starts_with("animate "))
        return true;
    const bool creation = value.starts_with("make ") || value.starts_with("create ") || value.starts_with("generate ") ||
                          value.starts_with("produce ") || value.starts_with("render ") || value.starts_with("turn ");
    return creation &&
           (Word(value, "video") || Word(value, "clip") || Word(value, "animation") || (Word(value, "image") && Word(value, "move")));
}
}
