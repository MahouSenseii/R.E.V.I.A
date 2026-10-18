#include "Workspace/projectShare.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"
#include "Memory/sensitiveContent.h"
#include "Policy/guestCapabilityPolicy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace revia::workspace
{
namespace
{
constexpr std::size_t ShownSkips = 40;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Version control, dependencies and build output: large, regenerable, and never what a
// review of the source needs.
bool SkippedFolder(const std::string& name)
{
    static constexpr std::array<std::string_view, 16> names = {".git", ".hg", ".svn", "node_modules", "__pycache__", ".venv", "venv",
        ".idea", ".vs", "build", "out", "dist", "target", "obj", ".pytest_cache", ".mypy_cache"};
    const std::string lower = Lower(name);
    return std::find(names.begin(), names.end(), lower) != names.end() || lower.rfind("cmake-build-", 0) == 0;
}

// Names that hold credentials by convention. Their content is not read at all.
bool SecretName(const std::string& name)
{
    const std::string lower = Lower(name);
    static constexpr std::array<std::string_view, 7> exact = {
        ".env", ".npmrc", ".pypirc", ".netrc", ".git-credentials", "credentials", "credentials.json"};
    static constexpr std::array<std::string_view, 6> suffixes = {".pem", ".key", ".pfx", ".p12", ".jks", ".keystore"};
    static constexpr std::array<std::string_view, 4> prefixes = {"id_rsa", "id_ed25519", "id_ecdsa", "id_dsa"};
    if (std::find(exact.begin(), exact.end(), lower) != exact.end() || lower.rfind(".env.", 0) == 0)
        return true;
    for (const auto suffix : suffixes)
        if (lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    return std::any_of(prefixes.begin(), prefixes.end(), [&lower](const std::string_view prefix) { return lower.rfind(prefix, 0) == 0; });
}

bool ReparsePoint([[maybe_unused]] const std::filesystem::path& path)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

// What one file costs in the bridge's ASCII JSON, at worst.
std::size_t EscapedBytes(const std::string& text)
{
    std::size_t size = 0;
    for (const unsigned char c : text)
        size += c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' ? 2 : c < 0x20 ? 6 : c >= 0x80 ? 3 : 1;
    return size;
}

bool ReadBounded(const std::filesystem::path& path, const std::size_t limit, std::string& content)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    content.assign(limit + 1, '\0');
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(stream.gcount()));
    return !stream.bad() && content.size() <= limit;
}
}

ProjectSharePlan PlanProjectShare(const std::filesystem::path& folder, const ProjectShareLimits& limits, const std::stop_token stop)
{
    ProjectSharePlan plan;
    const auto fail = [&plan](std::string error)
    {
        plan = {};
        plan.error = std::move(error);
        return plan;
    };
    const auto skip = [&plan](const std::string& path, const std::string& reason)
    {
        ++plan.skippedCount;
        if (plan.skipped.size() < ShownSkips)
            plan.skipped.push_back(path + ": " + reason);
    };
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error) || error || ReparsePoint(folder))
        return fail("The folder to share does not exist or is a link.");

    std::vector<GuestFile> batch;
    std::size_t batchBytes = 0;
    std::size_t entries = 0;
    std::filesystem::recursive_directory_iterator walk(folder, std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
        return fail("The folder to share could not be read.");
    for (const std::filesystem::recursive_directory_iterator end; walk != end; walk.increment(error))
    {
        if (error)
            return fail("The folder to share could not be read completely.");
        if (stop.stop_requested())
            return fail("Sharing was cancelled.");
        if (++entries > limits.maxEntries)
            return fail("The folder has more than " + std::to_string(limits.maxEntries) + " entries; share a smaller folder.");
        const auto& entry = *walk;
        std::string relative = actions::PathToUtf8(entry.path().lexically_relative(folder));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        const std::string name = actions::PathToUtf8(entry.path().filename());
        const auto status = entry.symlink_status(error);
        if (error || std::filesystem::is_symlink(status) || ReparsePoint(entry.path()))
        {
            // Never followed: a link could name anything on this computer.
            if (std::filesystem::is_directory(status))
                walk.disable_recursion_pending();
            skip(relative, "a link");
            error.clear();
            continue;
        }
        if (std::filesystem::is_directory(status))
        {
            if (SkippedFolder(name))
                walk.disable_recursion_pending();
            continue;
        }
        if (!std::filesystem::is_regular_file(status))
            continue;
        if (!policy::IsGuestRelativePath(relative))
        {
            skip(relative, "a name the guest cannot hold");
            continue;
        }
        if (SecretName(name))
        {
            skip(relative, "named like a credential");
            continue;
        }
        const auto size = entry.file_size(error);
        if (error || size > limits.maxFileBytes)
        {
            skip(relative, "larger than " + std::to_string(limits.maxFileBytes) + " bytes");
            error.clear();
            continue;
        }
        std::string content;
        if (!ReadBounded(entry.path(), limits.maxFileBytes, content))
        {
            skip(relative, "unreadable");
            continue;
        }
        if (content.find('\0') != std::string::npos || !utf8::IsValid(content))
        {
            skip(relative, "not UTF-8 text");
            continue;
        }
        if (memory::ContainsSensitiveContent(content))
        {
            skip(relative, "contains what looks like a credential");
            continue;
        }
        if (++plan.files > limits.maxFiles)
            return fail("The folder has more than " + std::to_string(limits.maxFiles) + " text files; share a smaller folder.");
        if ((plan.bytes += content.size()) > limits.maxTotalBytes)
            return fail("The folder's text files exceed " + std::to_string(limits.maxTotalBytes) + " bytes; share a smaller folder.");
        const std::size_t cost = EscapedBytes(relative) + EscapedBytes(content) + 32;
        if (!batch.empty() && (batchBytes + cost > limits.maxBatchBytes || batch.size() >= 512))
        {
            plan.batches.push_back(std::move(batch));
            batch.clear();
            batchBytes = 0;
        }
        batch.push_back({std::move(relative), std::move(content)});
        batchBytes += cost;
    }
    if (!batch.empty())
        plan.batches.push_back(std::move(batch));
    if (plan.files == 0)
        return fail("The folder has no text files that can be shared.");
    return plan;
}

} // namespace revia::workspace
