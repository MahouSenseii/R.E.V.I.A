#include "Core/localApiToken.h"

#include "Core/localApiKey.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace revia::core
{
namespace
{
std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return bytes.size() > 4096 ? std::string{} : bytes;
}

#ifdef _WIN32
std::string Protect(const std::string& token, const bool encrypt)
{
    DATA_BLOB input{static_cast<DWORD>(token.size()), reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))};
    DATA_BLOB output{};
    const BOOL done = encrypt ? CryptProtectData(&input, L"Revia local API token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
                              : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!done)
        return {};
    std::string bytes(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return bytes;
}
#endif
} // namespace

bool IsLocalApiToken(const std::string& token)
{
    return token.size() == 70 && token.rfind("revia-", 0) == 0 &&
           std::all_of(token.begin() + 6, token.end(), [](const unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string LoadOrCreateLocalApiToken(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    std::string stored = ReadBytes(path);
#ifdef _WIN32
    stored = stored.empty() ? std::string{} : Protect(stored, false);
#endif
    if (IsLocalApiToken(stored))
        return stored;

    const std::string token = GenerateLocalApiKey();
#ifdef _WIN32
    const std::string bytes = Protect(token, true);
    if (bytes.empty())
    {
        error = "Windows could not protect the local API token.";
        return {};
    }
#else
    const std::string& bytes = token;
#endif
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    const std::filesystem::path partial = path.string() + ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            error = "The local API token could not be saved.";
            return {};
        }
    }
    std::filesystem::permissions(partial, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ignored);
    std::error_code moved;
    std::filesystem::rename(partial, path, moved);
    if (moved)
    {
        std::filesystem::remove(partial, ignored);
        error = "The local API token could not be saved.";
        return {};
    }
    return token;
}

} // namespace revia::core
