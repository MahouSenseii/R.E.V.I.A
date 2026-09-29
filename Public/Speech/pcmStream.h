#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::speech
{

// Raw 16-bit mono PCM arriving in pieces, assembled into samples.
//
// A network chunk ends wherever the transport cut it, which is as likely as not in the
// middle of a sample; the odd byte is carried to the next chunk rather than dropped,
// because a dropped byte shifts every later sample by half and turns speech to noise.
class PcmStreamAssembler
{
public:
    void Append(const std::uint8_t* bytes, std::size_t count);
    void Append(const std::string& bytes);

    [[nodiscard]] const std::vector<std::int16_t>& Samples() const { return samples; }
    [[nodiscard]] std::size_t SampleCount() const { return samples.size(); }
    [[nodiscard]] double BufferedMilliseconds(int sampleRate) const;
    // A byte left over from the last chunk, waiting for its other half.
    [[nodiscard]] bool HasPendingByte() const { return pending; }

    // The samples as a canonical 16-bit mono WAV, which is what the playback path
    // already knows how to play.
    [[nodiscard]] std::vector<std::uint8_t> ToWav(int sampleRate) const;

private:
    std::vector<std::int16_t> samples;
    bool pending = false;
    std::uint8_t pendingByte = 0;
};

[[nodiscard]] std::vector<std::uint8_t> WrapPcmAsWav(
    const std::vector<std::int16_t>& samples, int sampleRate);

} // namespace revia::speech
