#include "Speech/playbackEnvelope.h"
#include "Performance/wavAudio.h"
#include "testSupport.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

namespace
{

using revia::speech::BuildPlaybackEnvelope;
using revia::tests::Check;

void AppendLittle(std::vector<std::uint8_t>& bytes, const std::uint32_t value, const int width)
{
    for (int index = 0; index < width; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

std::vector<std::uint8_t> PcmWav(const std::vector<std::int16_t>& samples, const int channels = 1)
{
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * 2);
    std::vector<std::uint8_t> bytes;
    const auto text = [&bytes](const char* value)
    {
        bytes.insert(bytes.end(), value, value + 4);
    };
    text("RIFF");
    AppendLittle(bytes, 36 + dataSize, 4);
    text("WAVE");
    text("fmt ");
    AppendLittle(bytes, 16, 4);
    AppendLittle(bytes, 1, 2);
    AppendLittle(bytes, channels, 2);
    AppendLittle(bytes, 8000, 4);
    AppendLittle(bytes, 16000 * channels, 4);
    AppendLittle(bytes, 2 * channels, 2);
    AppendLittle(bytes, 16, 2);
    text("data");
    AppendLittle(bytes, dataSize, 4);
    for (const auto sample : samples)
        AppendLittle(bytes, static_cast<std::uint16_t>(sample), 2);
    return bytes;
}

void TestMouthEnvelopeFollowsSilenceAndVoice()
{
    std::vector<std::int16_t> samples(1200, 0);
    std::fill(samples.begin() + 400, samples.begin() + 800, 8192);
    const auto envelope = BuildPlaybackEnvelope(PcmWav(samples));
    Check(envelope.has_value(), "Valid spoken PCM did not produce a playback envelope.");
    Check(envelope->values.size() == 3 && envelope->values[0] == 0 && envelope->values[1] == 255 && envelope->values[2] == 0,
        "The mouth envelope did not close during silence surrounding a voiced interval.");
}

void TestStereoEnergyDoesNotCancelOppositeChannels()
{
    std::vector<std::int16_t> samples;
    for (int frame = 0; frame < 400; ++frame)
    {
        samples.push_back(4096);
        samples.push_back(-4096);
    }
    const auto envelope = BuildPlaybackEnvelope(PcmWav(samples, 2));
    Check(envelope && envelope->values.size() == 1 && envelope->values[0] >= 139 && envelope->values[0] <= 160,
        "Opposite stereo channels cancelled instead of contributing independent loudness.");
}

void TestQuietAudioAndPartialWindow()
{
    const auto quiet = BuildPlaybackEnvelope(PcmWav(std::vector<std::int16_t>(400, 100)));
    Check(quiet && quiet->values.size() == 1 && quiet->values[0] == 0, "Near-silent output opened the mouth.");
    const auto partial = BuildPlaybackEnvelope(PcmWav(std::vector<std::int16_t>(401, 8192)));
    Check(partial && partial->values.size() == 2 && partial->values[1] == 255, "The final partial voiced window was dropped.");
}

void TestMalformedAndUnsupportedAudioHasNoEnvelope()
{
    Check(!BuildPlaybackEnvelope(std::vector<std::uint8_t>{'R', 'I', 'F', 'F'}), "Malformed audio received an envelope.");
    auto invalid = PcmWav(std::vector<std::int16_t>(400, 8192));
    invalid[20] = 2;
    Check(!BuildPlaybackEnvelope(invalid), "Unsupported encoded audio received an envelope.");
    invalid = PcmWav({});
    Check(!BuildPlaybackEnvelope(invalid), "Empty audio received an envelope.");
}

void TestEnvelopeDurationIsBounded()
{
    auto samples = std::vector<std::int16_t>(8000 * 120, 0);
    const auto maximum = BuildPlaybackEnvelope(PcmWav(samples));
    Check(maximum && maximum->values.size() == 2400, "A permitted two-minute WAV exceeded or missed its bounded envelope.");
    samples.push_back(0);
    Check(!BuildPlaybackEnvelope(PcmWav(samples)), "Audio beyond two minutes was admitted into the envelope.");
}

void TestDiskAndMemoryAudioShareEnvelopeDecoding()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "own-output.wav";
    const auto wav = PcmWav(std::vector<std::int16_t>(400, 8192));
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    }
    const auto disk = BuildPlaybackEnvelope(path);
    const auto memory = BuildPlaybackEnvelope(wav);
    Check(disk && memory && disk->values == memory->values && disk->values.front() == 255,
        "Disk-backed and in-memory speech did not produce the same spoken loudness.");
    Check(!BuildPlaybackEnvelope(directory.root / "missing.wav"), "A missing clip received an envelope.");
}

void TestMemoryDecoderSkipsPaddedMetadata()
{
    auto wav = PcmWav(std::vector<std::int16_t>(400, 8192));
    const std::vector<std::uint8_t> metadata = {'J', 'U', 'N', 'K', 3, 0, 0, 0, 'a', 'b', 'c', 0};
    wav.insert(wav.begin() + 12, metadata.begin(), metadata.end());
    const auto size = static_cast<std::uint32_t>(wav.size() - 8);
    for (int index = 0; index < 4; ++index)
        wav[4 + index] = static_cast<std::uint8_t>(size >> (index * 8));
    revia::performance::PcmAudio audio;
    std::string error;
    Check(revia::performance::ReadWavBytes(wav, audio, error, 120000) && audio.sampleRate == 8000 && audio.channels == 1 &&
            audio.FrameCount() == 400 && audio.samples.front() == 8192,
        "Borrowed WAV decoding did not skip an odd-sized metadata chunk and its padding.");
    const auto envelope = BuildPlaybackEnvelope(wav);
    Check(envelope && envelope->values.size() == 1 && envelope->values.front() == 255,
        "Padded metadata changed the measured speech loudness.");
}

} // namespace

void RunPlaybackEnvelopeTests()
{
    TestMouthEnvelopeFollowsSilenceAndVoice();
    TestStereoEnergyDoesNotCancelOppositeChannels();
    TestQuietAudioAndPartialWindow();
    TestMalformedAndUnsupportedAudioHasNoEnvelope();
    TestEnvelopeDurationIsBounded();
    TestDiskAndMemoryAudioShareEnvelopeDecoding();
    TestMemoryDecoderSkipsPaddedMetadata();
}
