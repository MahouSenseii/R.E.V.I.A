#include "Attachments/inflate.h"

#include <array>
#include <cstdint>

namespace revia::attachments
{
namespace
{
// Follows the structure of Mark Adler's reference decoder (zlib's contrib/puff): a
// canonical Huffman table is only symbol counts per length plus the symbols in code
// order, which keeps every table a fixed small array with nothing to overflow.
constexpr int MaxBits = 15;
constexpr int MaxLengthCodes = 286;
constexpr int MaxDistanceCodes = 30;
constexpr int FixedLengthCodes = 288;

struct Huffman
{
    std::array<short, MaxBits + 1> count{};
    std::array<short, FixedLengthCodes> symbol{};
};

// Returns 0 for a complete code, a positive count of unused codes for an incomplete
// one, and a negative value for an over-subscribed (invalid) one.
int Construct(Huffman& table, const short* lengths, const int symbols)
{
    table.count.fill(0);
    for (int symbol = 0; symbol < symbols; ++symbol)
        ++table.count[static_cast<std::size_t>(lengths[symbol])];
    if (table.count[0] == symbols)
        return 0;
    int left = 1;
    for (int length = 1; length <= MaxBits; ++length)
    {
        left <<= 1;
        left -= table.count[static_cast<std::size_t>(length)];
        if (left < 0)
            return left;
    }
    std::array<short, MaxBits + 1> offsets{};
    for (int length = 1; length < MaxBits; ++length)
        offsets[static_cast<std::size_t>(length + 1)] =
            static_cast<short>(offsets[static_cast<std::size_t>(length)] + table.count[static_cast<std::size_t>(length)]);
    for (int symbol = 0; symbol < symbols; ++symbol)
        if (lengths[symbol] != 0)
            table.symbol[static_cast<std::size_t>(offsets[static_cast<std::size_t>(lengths[symbol])]++)] = static_cast<short>(symbol);
    return left;
}

class Decoder
{
public:
    Decoder(const std::string_view source, const std::size_t limit) : input(source), maxOutput(limit) {}

    InflateStatus Run()
    {
        int last = 0;
        do
        {
            int type = 0;
            if (!Bits(1, last) || !Bits(2, type))
                return InflateStatus::Truncated;
            InflateStatus status = InflateStatus::Corrupt;
            if (type == 0)
                status = Stored();
            else if (type == 1)
                status = Fixed();
            else if (type == 2)
                status = Dynamic();
            if (status != InflateStatus::Complete)
                return status;
        } while (last == 0);
        return InflateStatus::Complete;
    }

    // Whole input bytes read so far. A partly used byte at the end of the final block
    // counts as read, which is where a zlib checksum begins.
    [[nodiscard]] std::size_t Consumed() const { return position; }

    std::string output;

private:
    bool Bits(const int need, int& value)
    {
        std::uint64_t buffer = bitBuffer;
        while (bitCount < need)
        {
            if (position == input.size())
                return false;
            buffer |= static_cast<std::uint64_t>(static_cast<unsigned char>(input[position++])) << bitCount;
            bitCount += 8;
        }
        bitBuffer = buffer >> need;
        bitCount -= need;
        value = static_cast<int>(buffer & ((std::uint64_t{1} << need) - 1));
        return true;
    }

    // A symbol, or -1 when the input ran out, or -2 for a code the table does not hold.
    int Decode(const Huffman& table)
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int length = 1; length <= MaxBits; ++length)
        {
            int bit = 0;
            if (!Bits(1, bit))
                return -1;
            code |= bit;
            const int count = table.count[static_cast<std::size_t>(length)];
            if (code - count < first)
                return table.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -2;
    }

    bool Put(const char character)
    {
        if (output.size() >= maxOutput)
            return false;
        output.push_back(character);
        return true;
    }

    InflateStatus Stored()
    {
        bitBuffer = 0;
        bitCount = 0;
        if (input.size() - position < 4)
            return InflateStatus::Truncated;
        const auto byte = [this](const std::size_t at) { return static_cast<unsigned>(static_cast<unsigned char>(input[at])); };
        const unsigned length = byte(position) | (byte(position + 1) << 8);
        const unsigned complement = byte(position + 2) | (byte(position + 3) << 8);
        position += 4;
        if (length != (~complement & 0xffffU))
            return InflateStatus::Corrupt;
        for (unsigned copied = 0; copied < length; ++copied)
        {
            if (position == input.size())
                return InflateStatus::Truncated;
            if (!Put(input[position++]))
                return InflateStatus::TooLarge;
        }
        return InflateStatus::Complete;
    }

    InflateStatus Codes(const Huffman& lengthCode, const Huffman& distanceCode)
    {
        static constexpr std::array<short, 29> LengthBase = {
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::array<short, 29> LengthExtra = {
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::array<short, 30> DistanceBase = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
            513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::array<short, 30> DistanceExtra = {
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;)
        {
            int symbol = Decode(lengthCode);
            if (symbol < 0)
                return symbol == -1 ? InflateStatus::Truncated : InflateStatus::Corrupt;
            if (symbol < 256)
            {
                if (!Put(static_cast<char>(symbol)))
                    return InflateStatus::TooLarge;
                continue;
            }
            if (symbol == 256)
                return InflateStatus::Complete;
            symbol -= 257;
            if (symbol >= 29)
                return InflateStatus::Corrupt;
            int extra = 0;
            if (!Bits(LengthExtra[static_cast<std::size_t>(symbol)], extra))
                return InflateStatus::Truncated;
            const int length = LengthBase[static_cast<std::size_t>(symbol)] + extra;
            symbol = Decode(distanceCode);
            if (symbol < 0)
                return symbol == -1 ? InflateStatus::Truncated : InflateStatus::Corrupt;
            if (symbol >= 30)
                return InflateStatus::Corrupt;
            if (!Bits(DistanceExtra[static_cast<std::size_t>(symbol)], extra))
                return InflateStatus::Truncated;
            const std::size_t distance = static_cast<std::size_t>(DistanceBase[static_cast<std::size_t>(symbol)]) + extra;
            if (distance > output.size())
                return InflateStatus::Corrupt;
            for (int copied = 0; copied < length; ++copied)
                if (!Put(output[output.size() - distance]))
                    return InflateStatus::TooLarge;
        }
    }

    InflateStatus Fixed()
    {
        std::array<short, FixedLengthCodes> lengths{};
        int symbol = 0;
        for (; symbol < 144; ++symbol)
            lengths[static_cast<std::size_t>(symbol)] = 8;
        for (; symbol < 256; ++symbol)
            lengths[static_cast<std::size_t>(symbol)] = 9;
        for (; symbol < 280; ++symbol)
            lengths[static_cast<std::size_t>(symbol)] = 7;
        for (; symbol < FixedLengthCodes; ++symbol)
            lengths[static_cast<std::size_t>(symbol)] = 8;
        Huffman lengthCode;
        Construct(lengthCode, lengths.data(), FixedLengthCodes);
        std::array<short, MaxDistanceCodes> distances{};
        distances.fill(5);
        Huffman distanceCode;
        Construct(distanceCode, distances.data(), MaxDistanceCodes);
        return Codes(lengthCode, distanceCode);
    }

    InflateStatus Dynamic()
    {
        static constexpr std::array<short, 19> Order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int lengthCount = 0;
        int distanceCount = 0;
        int codeCount = 0;
        if (!Bits(5, lengthCount) || !Bits(5, distanceCount) || !Bits(4, codeCount))
            return InflateStatus::Truncated;
        lengthCount += 257;
        distanceCount += 1;
        codeCount += 4;
        if (lengthCount > MaxLengthCodes || distanceCount > MaxDistanceCodes)
            return InflateStatus::Corrupt;

        std::array<short, MaxLengthCodes + MaxDistanceCodes> lengths{};
        for (int index = 0; index < codeCount; ++index)
        {
            int value = 0;
            if (!Bits(3, value))
                return InflateStatus::Truncated;
            lengths[static_cast<std::size_t>(Order[static_cast<std::size_t>(index)])] = static_cast<short>(value);
        }
        Huffman lengthCode;
        if (Construct(lengthCode, lengths.data(), 19) != 0)
            return InflateStatus::Corrupt;

        int index = 0;
        while (index < lengthCount + distanceCount)
        {
            int symbol = Decode(lengthCode);
            if (symbol < 0)
                return symbol == -1 ? InflateStatus::Truncated : InflateStatus::Corrupt;
            if (symbol < 16)
            {
                lengths[static_cast<std::size_t>(index++)] = static_cast<short>(symbol);
                continue;
            }
            short repeated = 0;
            int repeat = 0;
            if (symbol == 16)
            {
                if (index == 0)
                    return InflateStatus::Corrupt;
                repeated = lengths[static_cast<std::size_t>(index - 1)];
                if (!Bits(2, repeat))
                    return InflateStatus::Truncated;
                repeat += 3;
            }
            else if (symbol == 17)
            {
                if (!Bits(3, repeat))
                    return InflateStatus::Truncated;
                repeat += 3;
            }
            else
            {
                if (!Bits(7, repeat))
                    return InflateStatus::Truncated;
                repeat += 11;
            }
            if (index + repeat > lengthCount + distanceCount)
                return InflateStatus::Corrupt;
            while (repeat-- > 0)
                lengths[static_cast<std::size_t>(index++)] = repeated;
        }
        // Without an end-of-block code the block could never finish.
        if (lengths[256] == 0)
            return InflateStatus::Corrupt;
        int error = Construct(lengthCode, lengths.data(), lengthCount);
        if (error < 0 || (error > 0 && lengthCount - lengthCode.count[0] != 1))
            return InflateStatus::Corrupt;
        Huffman distanceCode;
        error = Construct(distanceCode, lengths.data() + lengthCount, distanceCount);
        if (error < 0 || (error > 0 && distanceCount - distanceCode.count[0] != 1))
            return InflateStatus::Corrupt;
        return Codes(lengthCode, distanceCode);
    }

    std::string_view input;
    std::size_t maxOutput = 0;
    std::size_t position = 0;
    std::uint64_t bitBuffer = 0;
    int bitCount = 0;
};

std::uint32_t Adler32(const std::string_view bytes)
{
    std::uint32_t low = 1;
    std::uint32_t high = 0;
    for (const char character : bytes)
    {
        low = (low + static_cast<unsigned char>(character)) % 65521U;
        high = (high + low) % 65521U;
    }
    return (high << 16) | low;
}
} // namespace

InflateResult InflateRaw(const std::string_view input, const std::size_t maxOutput)
{
    Decoder decoder(input, maxOutput);
    InflateResult result;
    result.status = decoder.Run();
    result.output = std::move(decoder.output);
    return result;
}

InflateResult InflateZlib(const std::string_view input, const std::size_t maxOutput)
{
    InflateResult result;
    if (input.size() < 2)
    {
        result.status = InflateStatus::Truncated;
        return result;
    }
    const auto method = static_cast<unsigned char>(input[0]);
    const auto flags = static_cast<unsigned char>(input[1]);
    // Deflate with a window no larger than 32 KiB, a valid header check, and no preset
    // dictionary (a PDF never supplies one).
    if ((method & 0x0fU) != 8 || (method >> 4) > 7 || ((method << 8) | flags) % 31 != 0 || (flags & 0x20U) != 0)
        return result;
    Decoder decoder(input.substr(2), maxOutput);
    result.status = decoder.Run();
    result.output = std::move(decoder.output);
    if (result.status != InflateStatus::Complete)
        return result;
    const std::size_t checksumAt = 2 + decoder.Consumed();
    if (input.size() >= checksumAt + 4)
    {
        std::uint32_t stored = 0;
        for (std::size_t index = 0; index < 4; ++index)
            stored = (stored << 8) | static_cast<unsigned char>(input[checksumAt + index]);
        if (stored != Adler32(result.output))
            result.status = InflateStatus::Corrupt;
    }
    return result;
}

} // namespace revia::attachments
