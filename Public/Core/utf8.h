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

// The code point of the well-formed sequence at offset. `width` must be SequenceAt's
// answer for the same offset and not zero.
[[nodiscard]] inline char32_t CodePointAt(const std::string_view text, const std::size_t offset, const std::size_t width)
{
    const auto byte = [&](const std::size_t index) { return static_cast<char32_t>(static_cast<unsigned char>(text[offset + index])); };
    switch (width)
    {
        case 2: return ((byte(0) & 0x1FU) << 6) | (byte(1) & 0x3FU);
        case 3: return ((byte(0) & 0x0FU) << 12) | ((byte(1) & 0x3FU) << 6) | (byte(2) & 0x3FU);
        case 4: return ((byte(0) & 0x07U) << 18) | ((byte(1) & 0x3FU) << 12) | ((byte(2) & 0x3FU) << 6) | (byte(3) & 0x3FU);
        default: return byte(0);
    }
}

inline void AppendCodePoint(std::string& output, const char32_t codePoint)
{
    if (codePoint < 0x80U)
    {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint < 0x800U)
    {
        output.push_back(static_cast<char>(0xC0U | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else if (codePoint < 0x10000U)
    {
        output.push_back(static_cast<char>(0xE0U | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

// Whether a code point outside ASCII can be part of a word or a name. Read by block, not
// from the Unicode tables: letters and marks of any script are, and the punctuation,
// space, symbol, emoji and private-use blocks are not. Enough to keep "André" one word
// and to stop a dash or a curly quote from joining two.
[[nodiscard]] inline bool IsWordCodePoint(const char32_t codePoint)
{
    if (codePoint < 0x80U)
        return (codePoint >= U'0' && codePoint <= U'9') || (codePoint >= U'A' && codePoint <= U'Z') ||
               (codePoint >= U'a' && codePoint <= U'z');
    if (codePoint < 0xC0U)
        return codePoint == 0xAAU || codePoint == 0xB5U || codePoint == 0xBAU;
    if (codePoint == 0xD7U || codePoint == 0xF7U)
        return false;
    if ((codePoint >= 0x2000U && codePoint <= 0x2BFFU) || (codePoint >= 0x2E00U && codePoint <= 0x2E7FU) ||
        (codePoint >= 0x3000U && codePoint <= 0x303FU) || (codePoint >= 0xD800U && codePoint <= 0xF8FFU) ||
        (codePoint >= 0xFE00U && codePoint <= 0xFE6FU) || (codePoint >= 0xFF00U && codePoint <= 0xFF0FU) ||
        (codePoint >= 0xFF1AU && codePoint <= 0xFF20U) || (codePoint >= 0xFF3BU && codePoint <= 0xFF40U) ||
        (codePoint >= 0xFF5BU && codePoint <= 0xFF65U) || (codePoint >= 0xFFF0U && codePoint <= 0xFFFFU) ||
        (codePoint >= 0x1D000U && codePoint <= 0x1FFFFU) || codePoint >= 0xE0000U)
        return false;
    return true;
}

// Lower case for the scripts that have one and are simple to fold: ASCII, Latin-1, Latin
// Extended-A, Greek and Cyrillic. One code point in, one out. Bytes that are not
// well-formed UTF-8 pass through untouched. Used so a name typed as "ÉLODIE", "Élodie"
// or "élodie" is one name; it is not a full Unicode case folding.
[[nodiscard]] inline char32_t FoldCodePoint(const char32_t codePoint)
{
    if (codePoint >= U'A' && codePoint <= U'Z')
        return codePoint + 0x20U;
    if ((codePoint >= 0xC0U && codePoint <= 0xDEU && codePoint != 0xD7U) || (codePoint >= 0x391U && codePoint <= 0x3ABU && codePoint != 0x3A2U) ||
        (codePoint >= 0x410U && codePoint <= 0x42FU))
        return codePoint + 0x20U;
    if (codePoint >= 0x400U && codePoint <= 0x40FU)
        return codePoint + 0x50U;
    if (codePoint == 0x178U)
        return 0xFFU;
    if ((codePoint >= 0x100U && codePoint <= 0x137U) || (codePoint >= 0x14AU && codePoint <= 0x177U))
        return codePoint | 1U;
    if ((codePoint >= 0x139U && codePoint <= 0x148U) || (codePoint >= 0x179U && codePoint <= 0x17EU))
        return (codePoint & 1U) != 0U ? codePoint + 1U : codePoint;
    return codePoint;
}

[[nodiscard]] inline std::string FoldCase(const std::string_view text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();)
    {
        const std::size_t width = SequenceAt(text, offset);
        if (width == 0)
        {
            output.push_back(text[offset++]);
            continue;
        }
        AppendCodePoint(output, FoldCodePoint(CodePointAt(text, offset, width)));
        offset += width;
    }
    return output;
}

} // namespace revia::utf8
