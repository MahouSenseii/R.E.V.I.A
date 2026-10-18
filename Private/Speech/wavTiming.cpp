#include "Speech/wavTiming.h"

#include <algorithm>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace revia::speech
{
namespace
{
std::uint32_t Read16(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t Read32(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    return Read16(bytes, offset) | (Read16(bytes, offset + 2) << 16U);
}

std::string_view Tag(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    return {reinterpret_cast<const char*>(bytes.data() + offset), 4};
}

// The first bytes of a file are enough: the format and data headers come before the audio.
constexpr std::size_t HeaderBytes = 4096;
} // namespace

double WavDurationMilliseconds(const std::span<const std::uint8_t> header, const std::uint64_t totalBytes)
{
    if (header.size() < 12 || Tag(header, 0) != "RIFF" || Tag(header, 8) != "WAVE")
        return 0.0;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockAlign = 0;
    std::size_t cursor = 12;
    while (cursor + 8 <= header.size())
    {
        const std::uint32_t chunkSize = Read32(header, cursor + 4);
        const std::size_t payload = cursor + 8;
        if (Tag(header, cursor) == "fmt " && chunkSize >= 16 && payload + 16 <= header.size())
        {
            sampleRate = Read32(header, payload + 4);
            blockAlign = Read16(header, payload + 12);
        }
        else if (Tag(header, cursor) == "data")
        {
            if (sampleRate == 0 || blockAlign == 0 || totalBytes <= payload)
                return 0.0;
            // A writer that streamed the file may leave the size as a placeholder.
            const std::uint64_t present = totalBytes - payload;
            const std::uint64_t dataBytes = chunkSize == 0xFFFFFFFFU ? present : std::min<std::uint64_t>(chunkSize, present);
            return static_cast<double>(dataBytes / blockAlign) * 1000.0 / static_cast<double>(sampleRate);
        }
        cursor = payload + static_cast<std::size_t>(chunkSize) + (chunkSize & 1U);
    }
    return 0.0;
}

double WavDurationMilliseconds(const std::span<const std::uint8_t> wav)
{
    return WavDurationMilliseconds(wav, wav.size());
}

double WavDurationMilliseconds(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
        return 0.0;
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> header(static_cast<std::size_t>(std::min<std::uintmax_t>(size, HeaderBytes)));
    if (!file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())))
        return 0.0;
    return WavDurationMilliseconds(header, size);
}

double PlaybackHoldMilliseconds(const double parsedMilliseconds, const double reportedMilliseconds)
{
    const double audio = std::max(parsedMilliseconds, reportedMilliseconds);
    return audio > 0.0 ? audio + PlaybackTailMilliseconds : 600.0;
}

} // namespace revia::speech
