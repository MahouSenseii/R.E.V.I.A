#include "Workspace/guestReceipt.h"

#include "Audit/contentDigest.h"

#include <stdexcept>

namespace revia::workspace
{
namespace
{
std::string Field(const nlohmann::json& item, const char* key, const std::size_t limit)
{
    if (!item.contains(key) || !item[key].is_string() || item[key].get_ref<const std::string&>().size() > limit)
        throw std::runtime_error("invalid_receipt");
    return item[key].get<std::string>();
}
}

bool DecodeBase64(const std::string& text, std::string& bytes)
{
    const auto value = [](const char c) -> int
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    bytes.clear();
    if (text.size() % 4 != 0) return false;
    for (std::size_t index = 0; index < text.size(); index += 4)
    {
        const bool last = index + 4 == text.size();
        const int padding = last ? (text[index + 3] == '=') + (text[index + 2] == '=') : 0;
        // Padding only at the very end, and "x=y=" is not padding.
        if (padding == 1 && text[index + 2] == '=') return false;
        int digits[4] = {};
        for (int offset = 0; offset < 4 - padding; ++offset)
            if ((digits[offset] = value(text[index + static_cast<std::size_t>(offset)])) < 0) return false;
        const unsigned combined = (static_cast<unsigned>(digits[0]) << 18) | (static_cast<unsigned>(digits[1]) << 12) |
            (static_cast<unsigned>(digits[2]) << 6) | static_cast<unsigned>(digits[3]);
        bytes += static_cast<char>((combined >> 16) & 0xff);
        if (padding < 2) bytes += static_cast<char>((combined >> 8) & 0xff);
        if (padding < 1) bytes += static_cast<char>(combined & 0xff);
    }
    return true;
}

std::vector<GuestArtifact> ParseGuestArtifacts(const nlohmann::json& receipt)
{
    std::vector<GuestArtifact> artifacts;
    if (!receipt.contains("artifacts")) return artifacts;
    const auto& list = receipt["artifacts"];
    if (!list.is_array() || list.size() > MaxGuestArtifacts) throw std::runtime_error("invalid_receipt");
    std::size_t total = 0;
    for (const auto& item : list)
    {
        if (!item.is_object() || !item.contains("bytes") || !item["bytes"].is_number_unsigned())
            throw std::runtime_error("invalid_receipt");
        GuestArtifact artifact;
        artifact.name = Field(item, "name", 104);
        artifact.sha256 = Field(item, "sha256", 64);
        const std::string encoded = Field(item, "contentBase64", (MaxGuestArtifactBytes / 3 + 1) * 4);
        if (artifact.name.empty() || !DecodeBase64(encoded, artifact.bytes) || item["bytes"].get<std::size_t>() != artifact.bytes.size() ||
            (total += artifact.bytes.size()) > MaxGuestArtifactBytes || audit::ContentDigest(artifact.bytes) != artifact.sha256)
            throw std::runtime_error("invalid_receipt");
        artifacts.push_back(std::move(artifact));
    }
    return artifacts;
}

} // namespace revia::workspace
