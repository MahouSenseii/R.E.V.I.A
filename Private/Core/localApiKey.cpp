#include "Core/localApiKey.h"

#include <array>
#include <iomanip>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace revia::core
{

std::string GenerateLocalApiKey()
{
    std::array<unsigned char, 32> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
#endif
    {
        std::random_device source;
        for (unsigned char& value : bytes)
        {
            value = static_cast<unsigned char>(source());
        }
    }

    std::ostringstream key;
    key << "revia-" << std::hex << std::setfill('0');
    for (const unsigned char value : bytes)
    {
        key << std::setw(2) << static_cast<unsigned int>(value);
    }
    return key.str();
}

} // namespace revia::core
