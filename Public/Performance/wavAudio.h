#pragma once

#include "Performance/songTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::performance
{

// Decoded audio, interleaved, always signed 16-bit because that is what the Windows
// wave-out path wants and converting once on the way in beats converting per buffer.
struct PcmAudio
{
    int sampleRate = 0;
    int channels = 0;
    std::vector<std::int16_t> samples;

    [[nodiscard]] std::int64_t FrameCount() const;
    [[nodiscard]] std::int64_t DurationMs() const;
    [[nodiscard]] bool Empty() const { return samples.empty(); }
};

// Reads a RIFF/WAVE file. Accepts 8/16/24/32-bit PCM and 32/64-bit IEEE float, mono or
// stereo, and converts everything to 16-bit. Anything else is refused by name rather
// than half-played, because a wrong guess about a header sounds like noise at full
// volume, which is not a failure mode to leave to chance.
//
// maximumFrames stops a file that claims an implausible length from being read into
// memory in full; pass 0 for no limit.
[[nodiscard]] bool ReadWavFile(
    const std::filesystem::path& path,
    PcmAudio& outAudio,
    std::string& outError,
    std::int64_t maximumFrames = 0);

// Duplicates a mono track across both channels. A mono vocal over a stereo backing
// track is the normal shape of a karaoke asset, so this is the common path, not a
// fallback.
[[nodiscard]] bool ConvertToStereo(PcmAudio& audio, std::string& outError);

// Sums overlay into base with independent gains, saturating rather than wrapping --
// wrapping turns a loud moment into a burst of noise. Returns how many samples hit the
// limit, so a mix that is quietly destroying itself can say so instead of just sounding
// bad. base is extended when overlay is longer.
std::size_t MixInto(
    std::vector<std::int16_t>& base,
    const std::vector<std::int16_t>& overlay,
    double baseGain,
    double overlayGain);

void ApplyGain(std::vector<std::int16_t>& samples, double gain);

// Where the vocal track is actually singing, derived from short-window loudness.
//
// thresholdRatio is a fraction of the track's own peak, so a quietly mastered vocal is
// not reported as silent. Spans closer together than mergeGapMs are joined: breathing
// between two words is not the end of a phrase, and an avatar that closed its mouth
// there would look broken.
[[nodiscard]] std::vector<VocalSpan> DetectVocalSpans(
    const PcmAudio& audio,
    double thresholdRatio = 0.06,
    std::int64_t windowMs = 40,
    std::int64_t mergeGapMs = 220);

} // namespace revia::performance
