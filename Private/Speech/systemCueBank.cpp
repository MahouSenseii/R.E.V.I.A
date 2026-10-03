#include "Speech/systemCueBank.h"
#include "Speech/vocalization.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace revia::speech
{
namespace
{
std::string CueId(const SystemCueKind kind)
{
    const auto& catalog = ApprovedSystemCues();
    const auto found = std::find_if(catalog.begin(), catalog.end(), [kind](const auto& cue) { return cue.kind == kind; });
    return found == catalog.end() ? std::string{} : std::string(found->id);
}

std::string ScopeDigest(const std::string& profileId, const VoicePreset& preset, const unsigned phraseVersion)
{
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    struct CloseHash
    {
        BCRYPT_ALG_HANDLE& algorithm;
        BCRYPT_HASH_HANDLE& hash;
        ~CloseHash()
        {
            if (hash)
                BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } close{algorithm, hash};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        return {};
    const auto append = [&](const std::string& value)
    {
        const std::string framed = std::to_string(value.size()) + ":" + value;
        return BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(framed.data())), static_cast<ULONG>(framed.size()), 0) >= 0;
    };
    if (!append(profileId) || !append(preset.id) || !append(preset.name) || !append(preset.description) || !append(preset.referenceText) ||
        !append(preset.createdAt) || !append(preset.language) || !append(std::to_string(phraseVersion)))
        return {};
    std::error_code error;
    const auto bytes = std::filesystem::file_size(preset.referenceAudioPath, error);
    constexpr std::uintmax_t maximumReferenceBytes = 32U * 1024U * 1024U;
    if (error || bytes == 0 || bytes > maximumReferenceBytes || !append(std::to_string(bytes)))
        return {};
    std::ifstream reference(preset.referenceAudioPath, std::ios::binary);
    std::array<char, 8192> buffer{};
    std::uintmax_t read = 0;
    while (reference && read < bytes)
    {
        reference.read(buffer.data(), static_cast<std::streamsize>(std::min<std::uintmax_t>(buffer.size(), bytes - read)));
        const auto count = reference.gcount();
        if (count <= 0 || BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0)
            return {};
        read += static_cast<std::uintmax_t>(count);
    }
    if (read != bytes || reference.peek() != std::char_traits<char>::eof())
        return {};
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        return {};
    std::ostringstream output;
    for (const auto byte : digest)
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return output.str();
#else
    (void)profileId;
    (void)preset;
    (void)phraseVersion;
    return {};
#endif
}
}

std::optional<SystemCueBank> SystemCueBank::ForVoice(
    const std::filesystem::path& root, const std::string& profileId, const VoicePreset& preset, const unsigned phraseVersion)
{
    if (root.empty() || profileId.empty() || preset.id.empty() || profileId.size() > 256 || preset.id.size() > 256 ||
        preset.name.size() > 256 || preset.description.size() > 1000 || preset.referenceText.size() > 1000 || preset.language.size() > 40 ||
        preset.createdAt.size() > 64)
        return std::nullopt;
    SystemCueBank bank;
    bank.key = ScopeDigest(profileId, preset, phraseVersion);
    if (bank.key.empty())
        return std::nullopt;
    bank.directory = root / "SystemCues" / bank.key;
    return bank;
}

const std::string& SystemCueBank::Key() const
{
    return key;
}
const std::filesystem::path& SystemCueBank::Directory() const
{
    return directory;
}

std::filesystem::path SystemCueBank::Clip(const SystemCueKind kind) const
{
    const std::string id = CueId(kind);
    if (id.empty())
        return {};
    const auto path = directory / (id + ".wav");
    return IsPlayableWavFile(path) ? path : std::filesystem::path{};
}

std::vector<SystemCueKind> SystemCueBank::MissingKinds() const
{
    std::vector<SystemCueKind> missing;
    for (const auto& cue : ApprovedSystemCues())
        if (Clip(cue.kind).empty())
            missing.push_back(cue.kind);
    return missing;
}

std::filesystem::path SystemCueBank::ScratchPath(const SystemCueKind kind) const
{
    static std::atomic<std::uint64_t> nextScratch = 0;
    const auto id = CueId(kind);
    if (id.empty())
        return {};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = 0;
#endif
    return directory / (id + "-" + std::to_string(process) + "-" + std::to_string(++nextScratch) + ".pending.wav");
}

bool SystemCueBank::Publish(const SystemCueKind kind, const std::filesystem::path& scratch) const
{
    const auto id = CueId(kind);
    const auto filename = scratch.filename().string();
    if (id.empty() || scratch.parent_path() != directory || !filename.starts_with(id + "-") || !filename.ends_with(".pending.wav") ||
        !IsPlayableWavFile(scratch))
        return false;
    const auto finalPath = directory / (id + ".wav");
#ifdef _WIN32
    return MoveFileExW(scratch.wstring().c_str(), finalPath.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(scratch, finalPath, error);
    return !error;
#endif
}

} // namespace revia::speech
