#include "Core/secretStore.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace revia::core
{

namespace
{
constexpr const char* StoreEntropy = "revia.secret-store.v1";
constexpr const char* Extension = ".dpapi";

std::vector<unsigned char> ReadFile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        error = "Could not open " + path.string() + ".";
        return {};
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace

SecretStore::SecretStore(std::filesystem::path inputDirectory, Codec inputCodec)
    : directory(std::move(inputDirectory)), codec(std::move(inputCodec))
{
}

const char* SecretStore::Entropy()
{
    return StoreEntropy;
}

bool SecretStore::SystemCodecAvailable()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

SecretStore::Codec SecretStore::SystemCodec()
{
    Codec codec;
#ifdef _WIN32
    const auto transform = [](const std::vector<unsigned char>& input, std::string& error,
        const bool protect) -> std::vector<unsigned char>
    {
        std::vector<unsigned char> entropyBytes(StoreEntropy, StoreEntropy + std::strlen(StoreEntropy));
        DATA_BLOB in{};
        in.cbData = static_cast<DWORD>(input.size());
        in.pbData = const_cast<BYTE*>(input.data());
        DATA_BLOB entropy{};
        entropy.cbData = static_cast<DWORD>(entropyBytes.size());
        entropy.pbData = entropyBytes.data();
        DATA_BLOB out{};
        const BOOL ok = protect
            ? CryptProtectData(&in, L"Revia secret", &entropy, nullptr, nullptr,
                CRYPTPROTECT_UI_FORBIDDEN, &out)
            : CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                CRYPTPROTECT_UI_FORBIDDEN, &out);
        if (!ok)
        {
            error = protect
                ? "Windows could not protect the secret (DPAPI error " +
                    std::to_string(GetLastError()) + ")."
                : "This Windows account cannot open the secret (DPAPI error " +
                    std::to_string(GetLastError()) + "); it was written by another account "
                    "or machine.";
            return {};
        }
        std::vector<unsigned char> result(out.pbData, out.pbData + out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return result;
    };
    codec.protect = [transform](const std::vector<unsigned char>& bytes, std::string& error)
    {
        return transform(bytes, error, true);
    };
    codec.unprotect = [transform](const std::vector<unsigned char>& blob, std::string& error)
    {
        return transform(blob, error, false);
    };
#else
    const auto refuse = [](const std::vector<unsigned char>&, std::string& error)
        -> std::vector<unsigned char>
    {
        error = "The secret store needs Windows DPAPI; on this platform set the key's "
                "environment variable instead.";
        return {};
    };
    codec.protect = refuse;
    codec.unprotect = refuse;
#endif
    return codec;
}

bool SecretStore::ValidName(const std::string& name)
{
    if (name.empty() || name.size() > 64) return false;
    for (const char character : name)
    {
        const unsigned char value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-') return false;
    }
    return true;
}

std::filesystem::path SecretStore::PathOf(const std::string& name) const
{
    return directory / (name + Extension);
}

bool SecretStore::Store(const std::string& name, const std::string& value, std::string& outError) const
{
    if (!ValidName(name))
    {
        outError = "\"" + name + "\" is not a secret name (letters, digits, '_' and '-').";
        return false;
    }
    if (value.empty())
    {
        outError = "An empty secret is not stored.";
        return false;
    }
    if (!codec.protect)
    {
        outError = "No codec can protect the secret.";
        return false;
    }
    const std::vector<unsigned char> bytes(value.begin(), value.end());
    const std::vector<unsigned char> blob = codec.protect(bytes, outError);
    if (blob.empty())
    {
        if (outError.empty()) outError = "The secret could not be protected.";
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        outError = "Could not create " + directory.string() + ": " + error.message();
        return false;
    }
    std::ofstream file(PathOf(name), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        outError = "Could not write " + PathOf(name).string() + ".";
        return false;
    }
    file.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    return file.good();
}

std::optional<std::string> SecretStore::Load(const std::string& name, std::string& outError) const
{
    if (!ValidName(name))
    {
        outError = "\"" + name + "\" is not a secret name.";
        return std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(PathOf(name), error))
    {
        outError = "No secret named \"" + name + "\" in " + directory.string() + ".";
        return std::nullopt;
    }
    if (!codec.unprotect)
    {
        outError = "No codec can open the secret.";
        return std::nullopt;
    }
    const std::vector<unsigned char> blob = ReadFile(PathOf(name), outError);
    if (blob.empty())
    {
        if (outError.empty()) outError = "The secret file is empty.";
        return std::nullopt;
    }
    const std::vector<unsigned char> bytes = codec.unprotect(blob, outError);
    if (bytes.empty())
    {
        if (outError.empty()) outError = "The secret could not be opened.";
        return std::nullopt;
    }
    return std::string(bytes.begin(), bytes.end());
}

bool SecretStore::Has(const std::string& name) const
{
    std::error_code error;
    return ValidName(name) && std::filesystem::is_regular_file(PathOf(name), error);
}

bool SecretStore::Forget(const std::string& name) const
{
    if (!ValidName(name)) return false;
    std::error_code error;
    return std::filesystem::remove(PathOf(name), error);
}

} // namespace revia::core
