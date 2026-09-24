#include "artifactDigest.h"

#include <array>
#include <cstdio>
#include <sstream>

namespace revia::computer
{

namespace
{

// FIPS 180-4 SHA-256. Written out rather than pulled in, because the alternative was a
// dependency for one eighty-line function on a build that already has more optional
// pieces than it wants.
constexpr std::array<std::uint32_t, 64> RoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

std::uint32_t RotateRight(const std::uint32_t value, const std::uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

} // namespace

std::string Sha256Hex(const std::string& input)
{
    std::array<std::uint32_t, 8> state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    std::string message = input;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(input.size()) * 8u;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64u != 56u) message.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        message.push_back(static_cast<char>((bitLength >> shift) & 0xffu));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64u)
    {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16u; ++index)
        {
            const std::size_t at = offset + index * 4u;
            schedule[index] =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(message[at])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(message[at + 1])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(message[at + 2])) << 8) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(message[at + 3])));
        }
        for (std::size_t index = 16u; index < 64u; ++index)
        {
            const std::uint32_t s0 = RotateRight(schedule[index - 15], 7) ^
                RotateRight(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
            const std::uint32_t s1 = RotateRight(schedule[index - 2], 17) ^
                RotateRight(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
            schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
        }

        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t index = 0; index < 64u; ++index)
        {
            const std::uint32_t s1 =
                RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 =
                h + s1 + choice + RoundConstants[index] + schedule[index];
            const std::uint32_t s0 =
                RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    std::ostringstream stream;
    for (const std::uint32_t word : state)
    {
        char buffer[9];
        std::snprintf(buffer, sizeof(buffer), "%08x", word);
        stream << buffer;
    }
    return stream.str();
}

std::string BehaviourDigestInput(
    const std::uint32_t featureVersion,
    const std::vector<std::string>& featureNames,
    const std::vector<double>& weights,
    const double abstainBelow,
    const std::vector<std::string>& applications,
    const std::vector<std::string>& intents)
{
    // Newline-separated and section-labelled, so that moving a value from one list to
    // another changes the digest. A bare concatenation would let "ab" + "c" and "a" +
    // "bc" hash the same.
    const auto number = [](const double value)
    {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.17g", value);
        return std::string(buffer);
    };

    std::ostringstream stream;
    stream << "feature_version=" << featureVersion << '\n';
    stream << "features\n";
    for (const std::string& name : featureNames) stream << name << '\n';
    stream << "weights\n";
    for (const double weight : weights) stream << number(weight) << '\n';
    stream << "abstain_below=" << number(abstainBelow) << '\n';
    stream << "applications\n";
    for (const std::string& application : applications) stream << application << '\n';
    stream << "intents\n";
    for (const std::string& intent : intents) stream << intent << '\n';
    return stream.str();
}

} // namespace revia::computer
