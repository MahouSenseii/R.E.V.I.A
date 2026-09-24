#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace revia::perception
{

struct ClipboardText
{
    // UTF-8. Empty when the clipboard holds no text: nothing, an image, files.
    std::string text;
    // More was copied than was read.
    bool truncated = false;
};

// The clipboard's text, at most `maxBytes` of it. Empty when the clipboard cannot be
// read, which is always the case outside Windows.
[[nodiscard]] std::optional<ClipboardText> ReadClipboardText(std::size_t maxBytes);

// Whether the user is asking about something they copied: "what's on my clipboard?",
// "fix the code I just copied". The clipboard is read only then, never in passing.
[[nodiscard]] bool AsksAboutClipboard(const std::string& input);

} // namespace revia::perception
