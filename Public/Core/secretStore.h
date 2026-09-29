#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace revia::core
{

// Where a credential lives: never settings.json, never a command, never a log.
//
// A key is written as a blob only the Windows account that wrote it can open -- DPAPI,
// CryptProtectData with this store's own entropy -- by Tools/SetAdvisorKey.ps1, whose
// .NET ProtectedData call produces the same blob, or by Store below. It is read back
// here into memory for the client that needs it and nowhere else. On a platform
// without DPAPI the store holds nothing and says so; an environment variable is the
// fallback the callers offer there.
class SecretStore
{
public:
    // Bytes to blob and back. An empty result with `error` set means failure. A seam
    // so the store's own behaviour is tested without the platform's codec.
    struct Codec
    {
        std::function<std::vector<unsigned char>(
            const std::vector<unsigned char>& bytes, std::string& error)> protect;
        std::function<std::vector<unsigned char>(
            const std::vector<unsigned char>& blob, std::string& error)> unprotect;
    };

    explicit SecretStore(std::filesystem::path directory, Codec codec = SystemCodec());

    // The platform's codec: DPAPI (current user, this store's entropy) on Windows, and
    // one that refuses with the reason elsewhere.
    [[nodiscard]] static Codec SystemCodec();
    [[nodiscard]] static bool SystemCodecAvailable();
    // The entropy the blobs are bound to, so the PowerShell writer binds to the same.
    [[nodiscard]] static const char* Entropy();
    // Letters, digits, '_' and '-': a name is a file name, never a path.
    [[nodiscard]] static bool ValidName(const std::string& name);

    bool Store(const std::string& name, const std::string& value, std::string& outError) const;
    // The value, or nothing with `outError` saying why: no such secret, or a blob this
    // account cannot open.
    [[nodiscard]] std::optional<std::string> Load(const std::string& name, std::string& outError) const;
    [[nodiscard]] bool Has(const std::string& name) const;
    bool Forget(const std::string& name) const;
    [[nodiscard]] std::filesystem::path PathOf(const std::string& name) const;
    [[nodiscard]] const std::filesystem::path& Directory() const { return directory; }

private:
    std::filesystem::path directory;
    Codec codec;
};

} // namespace revia::core
