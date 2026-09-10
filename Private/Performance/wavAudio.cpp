#include "Performance/wavAudio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace revia::performance
{

namespace
{

constexpr std::uint16_t FormatPcm = 1;
constexpr std::uint16_t FormatFloat = 3;
constexpr std::uint16_t FormatExtensible = 0xFFFE;

std::uint32_t ReadU32(const unsigned char* data)
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint16_t ReadU16(const unsigned char* data)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0]) | (static_cast<std::uint16_t>(data[1]) << 8));
}

std::int16_t Saturate(const double value)
{
    constexpr double Minimum = -32768.0;
    constexpr double Maximum = 32767.0;
    return static_cast<std::int16_t>(std::lround(std::clamp(value, Minimum, Maximum)));
}

// One decoded sample from whatever the file actually stores, normalized to 16-bit.
std::int16_t DecodeSample(
    const unsigned char* data,
    const std::uint16_t format,
    const std::uint16_t bitsPerSample)
{
    if (format == FormatFloat)
    {
        if (bitsPerSample == 32)
        {
            float value = 0.0F;
            std::memcpy(&value, data, sizeof(value));
            return Saturate(static_cast<double>(value) * 32767.0);
        }
        double value = 0.0;
        std::memcpy(&value, data, sizeof(value));
        return Saturate(value * 32767.0);
    }
    switch (bitsPerSample)
    {
        case 8:
            // 8-bit WAV is unsigned with 128 as silence, which is the one place in this
            // format where "zero" is not zero.
            return static_cast<std::int16_t>((static_cast<int>(data[0]) - 128) << 8);
        case 16:
        {
            std::int16_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }
        case 24:
        {
            const std::int32_t value =
                (static_cast<std::int32_t>(static_cast<std::int8_t>(data[2])) << 16) |
                (static_cast<std::int32_t>(data[1]) << 8) |
                static_cast<std::int32_t>(data[0]);
            return static_cast<std::int16_t>(value >> 8);
        }
        case 32:
        default:
        {
            std::int32_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            return static_cast<std::int16_t>(value >> 16);
        }
    }
}

} // namespace

std::int64_t PcmAudio::FrameCount() const
{
    return channels > 0
        ? static_cast<std::int64_t>(samples.size()) / channels : 0;
}

std::int64_t PcmAudio::DurationMs() const
{
    return sampleRate > 0 ? (FrameCount() * 1000) / sampleRate : 0;
}

bool ReadWavFile(
    const std::filesystem::path& path,
    PcmAudio& outAudio,
    std::string& outError,
    const std::int64_t maximumFrames)
{
    outAudio = {};
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
    {
        outError = "There is no readable file at " + path.filename().string() + ".";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        outError = "Could not open " + path.filename().string() + ".";
        return false;
    }

    unsigned char header[12]{};
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0)
    {
        outError = path.filename().string() + " is not a RIFF/WAVE file.";
        return false;
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    bool haveFormat = false;

    // Chunks in any order, and unknown chunks skipped rather than treated as an error:
    // real files carry LIST, fact, and encoder junk before the data.
    while (true)
    {
        unsigned char chunk[8]{};
        if (!file.read(reinterpret_cast<char*>(chunk), sizeof(chunk)))
        {
            break;
        }
        const std::uint32_t chunkSize = ReadU32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0)
        {
            if (chunkSize < 16)
            {
                outError = "The format chunk in " + path.filename().string() + " is too short.";
                return false;
            }
            std::vector<unsigned char> formatChunk(chunkSize);
            if (!file.read(reinterpret_cast<char*>(formatChunk.data()),
                    static_cast<std::streamsize>(chunkSize)))
            {
                outError = "The format chunk in " + path.filename().string() + " is truncated.";
                return false;
            }
            format = ReadU16(formatChunk.data());
            channels = ReadU16(formatChunk.data() + 2);
            sampleRate = ReadU32(formatChunk.data() + 4);
            bitsPerSample = ReadU16(formatChunk.data() + 14);
            if (format == FormatExtensible && chunkSize >= 40)
            {
                // WAVE_FORMAT_EXTENSIBLE hides the real format in the first two bytes
                // of its sub-format GUID.
                format = ReadU16(formatChunk.data() + 24);
            }
            haveFormat = true;
            if (chunkSize % 2 == 1) file.seekg(1, std::ios::cur);
            continue;
        }
        if (std::memcmp(chunk, "data", 4) == 0)
        {
            if (!haveFormat)
            {
                outError = path.filename().string() + " has audio data before its format.";
                return false;
            }
            const bool supportedFormat = format == FormatPcm || format == FormatFloat;
            const bool supportedDepth = format == FormatFloat
                ? (bitsPerSample == 32 || bitsPerSample == 64)
                : (bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 ||
                    bitsPerSample == 32);
            if (!supportedFormat || !supportedDepth)
            {
                outError = path.filename().string() + " uses an unsupported encoding (format " +
                    std::to_string(format) + ", " + std::to_string(bitsPerSample) +
                    "-bit). Save it as 16-bit PCM WAV.";
                return false;
            }
            if (channels < 1 || channels > 2)
            {
                outError = path.filename().string() + " has " + std::to_string(channels) +
                    " channels; mono or stereo is required.";
                return false;
            }
            if (sampleRate < 8000 || sampleRate > 192000)
            {
                outError = path.filename().string() + " has an unusable sample rate of " +
                    std::to_string(sampleRate) + " Hz.";
                return false;
            }

            const std::uint32_t bytesPerSample = bitsPerSample / 8U;
            const std::uint32_t frameBytes = bytesPerSample * channels;
            if (frameBytes == 0)
            {
                outError = path.filename().string() + " declares a zero-length frame.";
                return false;
            }
            std::int64_t frames = static_cast<std::int64_t>(chunkSize / frameBytes);
            if (maximumFrames > 0 && frames > maximumFrames)
            {
                outError = path.filename().string() + " is longer than the configured limit.";
                return false;
            }

            std::vector<unsigned char> raw(static_cast<std::size_t>(frames) * frameBytes);
            file.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(raw.size()));
            // A file whose header promises more than it contains is common enough that
            // refusing it would be unhelpful; the honest response is to keep what is
            // really there and play that.
            const std::int64_t readFrames =
                static_cast<std::int64_t>(file.gcount()) / frameBytes;
            frames = std::min(frames, readFrames);
            if (frames <= 0)
            {
                outError = path.filename().string() + " contains no audio.";
                return false;
            }

            outAudio.sampleRate = static_cast<int>(sampleRate);
            outAudio.channels = static_cast<int>(channels);
            outAudio.samples.resize(static_cast<std::size_t>(frames) * channels);
            for (std::int64_t frame = 0; frame < frames; ++frame)
            {
                for (std::uint32_t channel = 0; channel < channels; ++channel)
                {
                    const std::size_t offset =
                        static_cast<std::size_t>(frame) * frameBytes + channel * bytesPerSample;
                    outAudio.samples[static_cast<std::size_t>(frame) * channels + channel] =
                        DecodeSample(raw.data() + offset, format, bitsPerSample);
                }
            }
            outError.clear();
            return true;
        }
        // Unknown chunk. RIFF pads odd-sized chunks to an even boundary.
        file.seekg(static_cast<std::streamoff>(chunkSize + (chunkSize % 2)), std::ios::cur);
        if (!file)
        {
            break;
        }
    }

    outError = path.filename().string() + " has no audio data chunk.";
    return false;
}

bool ConvertToStereo(PcmAudio& audio, std::string& outError)
{
    if (audio.channels == 2)
    {
        outError.clear();
        return true;
    }
    if (audio.channels != 1)
    {
        outError = "Only mono and stereo audio can be prepared for playback.";
        return false;
    }
    std::vector<std::int16_t> stereo(audio.samples.size() * 2);
    for (std::size_t index = 0; index < audio.samples.size(); ++index)
    {
        stereo[index * 2] = audio.samples[index];
        stereo[index * 2 + 1] = audio.samples[index];
    }
    audio.samples = std::move(stereo);
    audio.channels = 2;
    outError.clear();
    return true;
}

void ApplyGain(std::vector<std::int16_t>& samples, const double gain)
{
    if (gain == 1.0)
    {
        return;
    }
    for (std::int16_t& sample : samples)
    {
        sample = Saturate(static_cast<double>(sample) * gain);
    }
}

std::size_t MixInto(
    std::vector<std::int16_t>& base,
    const std::vector<std::int16_t>& overlay,
    const double baseGain,
    const double overlayGain)
{
    if (overlay.size() > base.size())
    {
        base.resize(overlay.size(), 0);
    }
    std::size_t clipped = 0;
    for (std::size_t index = 0; index < base.size(); ++index)
    {
        const double overlayValue = index < overlay.size()
            ? static_cast<double>(overlay[index]) * overlayGain : 0.0;
        const double mixed = static_cast<double>(base[index]) * baseGain + overlayValue;
        if (mixed > 32767.0 || mixed < -32768.0)
        {
            ++clipped;
        }
        base[index] = Saturate(mixed);
    }
    return clipped;
}

std::vector<VocalSpan> DetectVocalSpans(
    const PcmAudio& audio,
    const double thresholdRatio,
    const std::int64_t windowMs,
    const std::int64_t mergeGapMs)
{
    std::vector<VocalSpan> spans;
    if (audio.sampleRate <= 0 || audio.channels <= 0 || audio.samples.empty())
    {
        return spans;
    }

    const std::int64_t windowFrames =
        std::max<std::int64_t>(1, (audio.sampleRate * windowMs) / 1000);
    const std::int64_t frames = audio.FrameCount();
    std::vector<double> loudness;
    loudness.reserve(static_cast<std::size_t>(frames / windowFrames + 1));

    double peak = 0.0;
    for (std::int64_t start = 0; start < frames; start += windowFrames)
    {
        const std::int64_t stop = std::min(start + windowFrames, frames);
        double sum = 0.0;
        for (std::int64_t frame = start; frame < stop; ++frame)
        {
            for (int channel = 0; channel < audio.channels; ++channel)
            {
                const double value = static_cast<double>(
                    audio.samples[static_cast<std::size_t>(frame) * audio.channels + channel]);
                sum += value * value;
            }
        }
        const double count = static_cast<double>((stop - start) * audio.channels);
        const double rms = count > 0.0 ? std::sqrt(sum / count) : 0.0;
        loudness.push_back(rms);
        peak = std::max(peak, rms);
    }
    if (peak <= 0.0)
    {
        return spans;
    }

    // Relative to the track's own peak, so a quietly mastered vocal is not mistaken for
    // silence and a hot one does not report the whole song as singing.
    const double threshold = peak * std::clamp(thresholdRatio, 0.001, 0.9);
    const std::int64_t windowDurationMs = (windowFrames * 1000) / audio.sampleRate;
    bool active = false;
    std::int64_t spanStart = 0;
    for (std::size_t index = 0; index < loudness.size(); ++index)
    {
        const bool loud = loudness[index] >= threshold;
        const std::int64_t positionMs = static_cast<std::int64_t>(index) * windowDurationMs;
        if (loud && !active)
        {
            active = true;
            spanStart = positionMs;
        }
        else if (!loud && active)
        {
            active = false;
            spans.push_back({spanStart, positionMs});
        }
    }
    if (active)
    {
        spans.push_back({spanStart, audio.DurationMs()});
    }

    // A breath between two words is not the end of a phrase.
    std::vector<VocalSpan> merged;
    for (const VocalSpan& span : spans)
    {
        if (!merged.empty() && span.startMs - merged.back().endMs <= mergeGapMs)
        {
            merged.back().endMs = span.endMs;
            continue;
        }
        merged.push_back(span);
    }
    return merged;
}

} // namespace revia::performance
