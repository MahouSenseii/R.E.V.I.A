#include "Speech/playbackEnvelope.h"
#include "Performance/wavAudio.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace revia::speech
{

namespace
{

constexpr std::int64_t MaximumDurationMs = 120000;
constexpr std::size_t MaximumEnvelopeValues = 2400;

std::optional<PlaybackEnvelope> EnvelopeFor(const performance::PcmAudio& audio)
{
    const auto frames = audio.FrameCount();
    if (audio.sampleRate <= 0 || audio.channels <= 0 || frames <= 0 || frames > static_cast<std::int64_t>(audio.sampleRate) * 120)
        return std::nullopt;
    PlaybackEnvelope result;
    result.values.reserve(MaximumEnvelopeValues);
    for (std::int64_t window = 0; window < static_cast<std::int64_t>(MaximumEnvelopeValues); ++window)
    {
        const std::int64_t start = window * result.intervalMs * audio.sampleRate / 1000;
        if (start >= frames)
            break;
        const std::int64_t end = std::min(frames, (window + 1) * result.intervalMs * audio.sampleRate / 1000);
        double sum = 0.0;
        for (std::int64_t frame = start; frame < end; ++frame)
        {
            for (int channel = 0; channel < audio.channels; ++channel)
            {
                const double sample = static_cast<double>(audio.samples[static_cast<std::size_t>(frame) * audio.channels + channel]) / 32768.0;
                sum += sample * sample;
            }
        }
        const double rms = std::sqrt(sum / static_cast<double>((end - start) * audio.channels));
        const double mouth = std::clamp((rms - 0.015) * 5.0, 0.0, 1.0);
        result.values.push_back(static_cast<std::uint8_t>(std::lround(mouth * 255.0)));
    }
    return result;
}

} // namespace

std::optional<PlaybackEnvelope> BuildPlaybackEnvelope(const std::span<const std::uint8_t> wav)
{
    performance::PcmAudio audio;
    std::string error;
    if (!performance::ReadWavBytes(wav, audio, error, MaximumDurationMs))
        return std::nullopt;
    return EnvelopeFor(audio);
}

std::optional<PlaybackEnvelope> BuildPlaybackEnvelope(const std::filesystem::path& path)
{
    performance::PcmAudio audio;
    std::string error;
    if (!performance::ReadWavFile(path, audio, error, MaximumDurationMs))
        return std::nullopt;
    return EnvelopeFor(audio);
}

} // namespace revia::speech
