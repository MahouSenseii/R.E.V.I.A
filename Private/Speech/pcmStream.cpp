#include "Speech/pcmStream.h"

namespace revia::speech
{

void PcmStreamAssembler::Append(const std::uint8_t* bytes, const std::size_t count)
{
    std::size_t index = 0;
    if (pending && count > 0)
    {
        samples.push_back(static_cast<std::int16_t>(
            static_cast<std::uint16_t>(pendingByte) |
            (static_cast<std::uint16_t>(bytes[0]) << 8)));
        pending = false;
        index = 1;
    }
    for (; index + 1 < count; index += 2)
    {
        samples.push_back(static_cast<std::int16_t>(
            static_cast<std::uint16_t>(bytes[index]) |
            (static_cast<std::uint16_t>(bytes[index + 1]) << 8)));
    }
    if (index < count)
    {
        pending = true;
        pendingByte = bytes[index];
    }
}

void PcmStreamAssembler::Append(const std::string& bytes)
{
    Append(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

double PcmStreamAssembler::BufferedMilliseconds(const int sampleRate) const
{
    if (sampleRate <= 0) return 0.0;
    return static_cast<double>(samples.size()) * 1000.0 / static_cast<double>(sampleRate);
}

std::vector<std::uint8_t> PcmStreamAssembler::ToWav(const int sampleRate) const
{
    return WrapPcmAsWav(samples, sampleRate);
}

std::vector<std::uint8_t> WrapPcmAsWav(
    const std::vector<std::int16_t>& samples, const int sampleRate)
{
    std::vector<std::uint8_t> wav;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    wav.reserve(44 + dataBytes);
    const auto put32 = [&wav](const std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            wav.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
        }
    };
    const auto put16 = [&wav](const std::uint16_t value)
    {
        wav.push_back(static_cast<std::uint8_t>(value & 0xFF));
        wav.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto tag = [&wav](const char* text)
    {
        for (int index = 0; index < 4; ++index) wav.push_back(static_cast<std::uint8_t>(text[index]));
    };
    const std::uint32_t rate = static_cast<std::uint32_t>(sampleRate > 0 ? sampleRate : 24000);
    tag("RIFF");
    put32(36 + dataBytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);
    put16(1);
    put32(rate);
    put32(rate * 2);
    put16(2);
    put16(16);
    tag("data");
    put32(dataBytes);
    for (const std::int16_t sample : samples)
    {
        put16(static_cast<std::uint16_t>(sample));
    }
    return wav;
}

} // namespace revia::speech
