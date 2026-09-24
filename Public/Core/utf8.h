#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace revia::utf8
{

// The length of the well-formed sequence starting at offset, or zero when there is none.
// Rejects overlong encodings, surrogate code points and values above U+10FFFF.
[[nodiscard]] inline std::size_t SequenceAt(const std::string_view text, const std::size_t offset)
{
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80U) return 1;
    const std::size_t width = first >= 0xC2U && first <= 0xDFU ? 2 :
        first >= 0xE0U && first <= 0xEFU ? 3 :
        first >= 0xF0U && first <= 0xF4U ? 4 : 0;
    if (width == 0 || text.size() - offset < width) return 0;
    for (std::size_t index = 1; index < width; ++index)
        if ((static_cast<unsigned char>(text[offset + index]) & 0xC0U) != 0x80U)
            return 0;
    const auto second = static_cast<unsigned char>(text[offset + 1]);
    if ((first == 0xE0U && second < 0xA0U) ||
        (first == 0xEDU && second >= 0xA0U) ||
        (first == 0xF0U && second < 0x90U) ||
        (first == 0xF4U && second >= 0x90U)) return 0;
    return width;
}

// Validation and truncation are separate: a byte boundary cannot repair malformed
// input.
[[nodiscard]] inline bool IsValid(const std::string_view text)
{
    for (std::size_t offset = 0; offset < text.size();)
    {
        const std::size_t width = SequenceAt(text, offset);
        if (width == 0) return false;
        offset += width;
    }
    return true;
}

// The same text with every malformed byte replaced by U+FFFD, so the result is always
// valid UTF-8. For text from outside -- a file, a process -- that has to reach JSON.
[[nodiscard]] inline std::string Sanitize(const std::string_view text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();)
    {
        const std::size_t width = SequenceAt(text, offset);
        if (width == 0)
        {
            output += "\xEF\xBF\xBD";
            ++offset;
            continue;
        }
        output.append(text.substr(offset, width));
        offset += width;
    }
    return output;
}

// Largest complete-code-point prefix within a byte budget. Input must be valid UTF-8;
// callers accepting untrusted bytes validate separately. This is not grapheme layout.
[[nodiscard]] inline std::size_t PrefixBytes(const std::string_view text, const std::size_t maximum)
{
    std::size_t boundary = std::min(text.size(), maximum);
    while (boundary > 0 && boundary < text.size() &&
        (static_cast<unsigned char>(text[boundary]) & 0xC0U) == 0x80U) --boundary;
    return boundary;
}

inline void Truncate(std::string& text, const std::size_t maximum)
{
    text.resize(PrefixBytes(text, maximum));
}

[[nodiscard]] inline std::string Prefix(const std::string_view text, const std::size_t maximum)
{
    return std::string(text.substr(0, PrefixBytes(text, maximum)));
}

} // namespace revia::utf8
