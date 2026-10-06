#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace revia::speech
{

struct PlaybackEnvelope
{
    int intervalMs = 50;
    std::int64_t startedAtUnixMs = 0;
    std::vector<std::uint8_t> values;
};

// Bounded loudness from Revia's own WAV. The playback owner supplies its start time;
// PlaySound submission timing estimates playback and does not measure a device cursor.
[[nodiscard]] std::optional<PlaybackEnvelope> BuildPlaybackEnvelope(std::span<const std::uint8_t> wav);
[[nodiscard]] std::optional<PlaybackEnvelope> BuildPlaybackEnvelope(const std::filesystem::path& path);

} // namespace revia::speech
