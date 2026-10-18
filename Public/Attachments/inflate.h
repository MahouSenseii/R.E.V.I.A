#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace revia::attachments
{

enum class InflateStatus
{
    Complete,
    // The input ended before the final block. What was decoded so far is returned.
    Truncated,
    Corrupt,
    // Decoding would have produced more than the caller's limit; output stops there.
    TooLarge
};

struct InflateResult
{
    InflateStatus status = InflateStatus::Corrupt;
    std::string output;
};

// A DEFLATE decoder (RFC 1951) for the two places attachments need one: DOCX entries and
// PDF FlateDecode streams. There is no zlib in this build, and files handed over by the
// user are untrusted, so every read is bounds-checked and the output never grows past
// `maxOutput`; a compression bomb stops at the limit instead of filling memory.
[[nodiscard]] InflateResult InflateRaw(std::string_view input, std::size_t maxOutput);

// The zlib wrapper (RFC 1950) around DEFLATE, as PDF streams use it. The checksum is
// verified when the stream is complete.
[[nodiscard]] InflateResult InflateZlib(std::string_view input, std::size_t maxOutput);

} // namespace revia::attachments
