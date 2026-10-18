#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace revia::speech
{

// Silence held after a phrase's audio before the next phrase may start. PlaySound is
// asynchronous, so the playback thread waits out the clip itself; this only absorbs
// timer granularity. It was 350 ms, which put a third of a second of dead air between
// every pair of sentences.
inline constexpr double PlaybackTailMilliseconds = 60.0;

// The clip's length from its header: whole sample frames in the data chunk over the
// sample rate, with a data size larger than the bytes present clamped to what is
// there. Zero when the header cannot be read.
//
// `header` is the start of the file; `totalBytes` is the whole file's size, which may
// be larger than the header span when only the first bytes of a file were read.
[[nodiscard]] double WavDurationMilliseconds(std::span<const std::uint8_t> header, std::uint64_t totalBytes);
[[nodiscard]] double WavDurationMilliseconds(std::span<const std::uint8_t> wav);
[[nodiscard]] double WavDurationMilliseconds(const std::filesystem::path& path);

// How long to hold the channel after asynchronous playback starts. The longer of the
// parsed and reported lengths, so a short report can never end playback early and
// free an in-memory buffer the device is still reading. A clip whose length is unknown
// keeps the old fixed hold.
[[nodiscard]] double PlaybackHoldMilliseconds(double parsedMilliseconds, double reportedMilliseconds);

} // namespace revia::speech
