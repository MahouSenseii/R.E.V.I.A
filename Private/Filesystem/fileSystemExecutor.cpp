#include "Filesystem/fileSystemExecutor.h"

#include "Core/utf8.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <optional>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

// Win32's generic-name macros collide with Revia's typed action names.
#undef CopyFile
#undef CreateDirectory
#undef MoveFile
#endif

namespace revia::filesystem
{

namespace
{

actions::ActionResult Failure(std::string message, bool attempted = false)
{
    actions::ActionResult result;
    result.attempted = attempted;
    result.message = std::move(message);
    return result;
}

actions::ActionResult DryRunSuccess(const std::string& message)
{
    actions::ActionResult result;
    result.succeeded = true;
    result.dryRun = true;
    result.message = message;
    return result;
}

bool HasNullByte(const std::string& value)
{
    return value.find('\0') != std::string::npos;
}

void AppendUtf8(std::string& output, const std::uint32_t codePoint)
{
    if (codePoint < 0x80U)
    {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint < 0x800U)
    {
        output.push_back(static_cast<char>(0xC0U | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else if (codePoint < 0x10000U)
    {
        output.push_back(static_cast<char>(0xE0U | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

// Notepad saved "Unicode" as UTF-16 with a byte-order mark for decades. Its zero bytes
// read as binary, so an ordinary text file was refused as one. Unpaired surrogates and a
// stray final byte become U+FFFD.
std::optional<std::string> DecodeUtf16WithByteOrderMark(const std::string& bytes)
{
    if (bytes.size() < 2) return std::nullopt;
    const auto byte = [&bytes](const std::size_t index)
    {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
    };
    bool bigEndian = false;
    if (byte(0) == 0xFFU && byte(1) == 0xFEU) bigEndian = false;
    else if (byte(0) == 0xFEU && byte(1) == 0xFFU) bigEndian = true;
    else return std::nullopt;

    const auto unitAt = [&](const std::size_t index)
    {
        return bigEndian ? (byte(index) << 8) | byte(index + 1)
                         : byte(index) | (byte(index + 1) << 8);
    };
    constexpr std::uint32_t Replacement = 0xFFFDU;
    std::string output;
    output.reserve(bytes.size());
    std::size_t index = 2;
    for (; index + 1 < bytes.size(); index += 2)
    {
        const std::uint32_t unit = unitAt(index);
        if (unit >= 0xD800U && unit <= 0xDBFFU && index + 3 < bytes.size())
        {
            const std::uint32_t low = unitAt(index + 2);
            if (low >= 0xDC00U && low <= 0xDFFFU)
            {
                AppendUtf8(output, 0x10000U + ((unit - 0xD800U) << 10) + (low - 0xDC00U));
                index += 2;
                continue;
            }
        }
        AppendUtf8(output, unit >= 0xD800U && unit <= 0xDFFFU ? Replacement : unit);
    }
    if (index < bytes.size()) AppendUtf8(output, Replacement);
    return output;
}

// Text that is not UTF-8 is, on Windows, almost always in the ANSI code page -- what
// Notepad wrote before it defaulted to UTF-8. Decoded from that where the platform can;
// otherwise, and for anything still malformed, U+FFFD stands in. Everything a read
// returns reaches JSON sooner or later, and JSON refuses malformed UTF-8.
std::string FromLegacyText(const std::string& bytes)
{
#ifdef _WIN32
    if (!bytes.empty() && bytes.size() <= static_cast<std::size_t>(INT_MAX))
    {
        const int length = static_cast<int>(bytes.size());
        const int wideLength = MultiByteToWideChar(CP_ACP, 0, bytes.data(), length, nullptr, 0);
        if (wideLength > 0)
        {
            std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
            MultiByteToWideChar(CP_ACP, 0, bytes.data(), length, wide.data(), wideLength);
            const int utf8Length = WideCharToMultiByte(
                CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
            if (utf8Length > 0)
            {
                std::string converted(static_cast<std::size_t>(utf8Length), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
                    converted.data(), utf8Length, nullptr, nullptr);
                return utf8::Sanitize(converted);
            }
        }
    }
#endif
    return utf8::Sanitize(bytes);
}

} // namespace

FileSystemExecutor::FileSystemExecutor(
    std::uintmax_t inputMaxReadBytes,
    std::size_t inputMaxDirectoryEntries,
    std::size_t inputMaxAffectedEntries)
    : maxReadBytes(inputMaxReadBytes),
      maxDirectoryEntries(inputMaxDirectoryEntries),
      maxAffectedEntries(inputMaxAffectedEntries)
{
}

bool FileSystemExecutor::Handles(actions::ActionType type) const
{
    return type == actions::ActionType::ListDirectory ||
        type == actions::ActionType::ReadTextFile ||
        type == actions::ActionType::CreateDirectory ||
        type == actions::ActionType::CopyFile ||
        type == actions::ActionType::MoveFile ||
        type == actions::ActionType::RenamePath ||
        type == actions::ActionType::MoveToRecycleBin;
}

actions::ActionResult FileSystemExecutor::Execute(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision)
{
    switch (request.type)
    {
        case actions::ActionType::ListDirectory:
            return ListDirectory(request, decision);
        case actions::ActionType::ReadTextFile:
            return ReadTextFile(request, decision);
        case actions::ActionType::CreateDirectory:
            return CreateDirectory(request, decision);
        case actions::ActionType::CopyFile:
            return CopyFile(request, decision);
        case actions::ActionType::MoveFile:
        case actions::ActionType::RenamePath:
            return MovePath(request, decision);
        case actions::ActionType::MoveToRecycleBin:
            return MoveToRecycleBin(request, decision);
        case actions::ActionType::Unknown:
        default:
            return Failure("The filesystem executor received an unsupported action.");
    }
}

actions::ActionResult FileSystemExecutor::ListDirectory(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision) const
{
    const auto& path = decision.canonicalSource;
    std::error_code error;
    if (!std::filesystem::is_directory(path, error) || error)
    {
        return Failure("Directory does not exist or is not accessible: " + actions::PathToUtf8(path));
    }

    if (request.dryRun)
    {
        return DryRunSuccess("Would list directory: " + actions::PathToUtf8(path));
    }

    actions::ActionResult result;
    result.attempted = true;
    std::filesystem::directory_iterator iterator(path, error);
    if (error)
    {
        result.message = "Could not open directory: " + error.message();
        return result;
    }

    const std::filesystem::directory_iterator end;
    while (iterator != end)
    {
        const auto& entry = *iterator;
        if (result.entries.size() >= maxDirectoryEntries)
        {
            result.entries.emplace_back("[LIMIT] Additional entries were omitted.");
            break;
        }

        std::string prefix = "[OTHER] ";
        const auto status = entry.symlink_status(error);
        if (error)
        {
            error.clear();
        }
        else if (std::filesystem::is_symlink(status))
        {
            prefix = "[LINK]  ";
        }
        else if (std::filesystem::is_directory(status))
        {
            prefix = "[DIR]   ";
        }
        else if (std::filesystem::is_regular_file(status))
        {
            prefix = "[FILE]  ";
        }
        result.entries.push_back(prefix + actions::PathToUtf8(entry.path().filename()));

        iterator.increment(error);
        if (error)
        {
            result.message = "Directory enumeration failed: " + error.message();
            return result;
        }
    }

    std::sort(result.entries.begin(), result.entries.end());
    result.succeeded = true;
    result.message = "Listed " + std::to_string(result.entries.size()) +
        " entries from " + actions::PathToUtf8(path);
    return result;
}

actions::ActionResult FileSystemExecutor::ReadTextFile(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision) const
{
    const auto& path = decision.canonicalSource;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        return Failure("Text file does not exist or is not a regular file: " + actions::PathToUtf8(path));
    }

    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
    {
        return Failure("Could not determine file size: " + error.message());
    }
    if (size > maxReadBytes)
    {
        return Failure("File exceeds the configured read limit of " +
            std::to_string(maxReadBytes) + " bytes.");
    }

    if (request.dryRun)
    {
        return DryRunSuccess("Would read text file: " + actions::PathToUtf8(path));
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return Failure("Could not open text file: " + actions::PathToUtf8(path), true);
    }

    auto result = ReadTextStream(file);
    if (result.succeeded)
    {
        result.message = "Read " + std::to_string(result.content.size()) +
            " bytes from " + actions::PathToUtf8(path);
    }
    return result;
}

actions::ActionResult FileSystemExecutor::ReadTextStream(std::istream& file) const
{
    std::string content;
    std::array<char, 4096> buffer;
    while (content.size() < maxReadBytes)
    {
        const auto count = static_cast<std::streamsize>(std::min<std::uintmax_t>(
            buffer.size(), maxReadBytes - content.size()));
        file.read(buffer.data(), count);
        if (file.bad() || (file.fail() && !file.eof()))
        {
            return Failure("Could not read text file.", true);
        }
        const auto read = static_cast<std::size_t>(file.gcount());
        if (read > content.max_size() - content.size())
        {
            return Failure("Text file exceeds the supported content size.", true);
        }
        content.append(buffer.data(), read);
        if (file.eof()) break;
    }
    // Probe one byte, without calculating maxReadBytes + 1 (which can overflow).
    // A growing file is rejected, never returned as a successful truncated read.
    if (content.size() == maxReadBytes && file.get() != std::char_traits<char>::eof())
    {
        return Failure("File exceeds the configured read limit of " +
            std::to_string(maxReadBytes) + " bytes.", true);
    }
    if (file.bad() || (file.fail() && !file.eof()))
    {
        return Failure("Could not read text file.", true);
    }
    if (std::optional<std::string> decoded = DecodeUtf16WithByteOrderMark(content))
    {
        content = std::move(*decoded);
    }
    if (HasNullByte(content))
    {
        return Failure("File appears to be binary and was not displayed.", true);
    }
    if (!utf8::IsValid(content))
    {
        content = FromLegacyText(content);
    }

    actions::ActionResult result;
    result.attempted = true;
    result.succeeded = true;
    result.content = std::move(content);
    return result;
}

actions::ActionResult FileSystemExecutor::CreateDirectory(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision) const
{
    const auto& path = decision.canonicalSource;
    if (request.dryRun)
    {
        return DryRunSuccess("Would create directory: " + actions::PathToUtf8(path));
    }

    actions::ActionResult result;
    result.attempted = true;
    std::error_code error;
    const bool created = std::filesystem::create_directories(path, error);
    if (error)
    {
        result.message = "Could not create directory: " + error.message();
        return result;
    }
    if (!created && !std::filesystem::is_directory(path, error))
    {
        result.message = "The target exists and is not a directory.";
        return result;
    }

    result.succeeded = true;
    result.message = created
        ? "Created directory: " + actions::PathToUtf8(path)
        : "Directory already exists: " + actions::PathToUtf8(path);
    return result;
}

actions::ActionResult FileSystemExecutor::CopyFile(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision) const
{
    const auto& source = decision.canonicalSource;
    const auto& destination = decision.canonicalDestination;
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error)
    {
        return Failure("Copy currently supports regular files only.");
    }
    if (std::filesystem::exists(destination, error))
    {
        return Failure("Copy refused because the destination already exists.");
    }

    if (request.dryRun)
    {
        return DryRunSuccess("Would copy " + actions::PathToUtf8(source) +
            " to " + actions::PathToUtf8(destination));
    }

    actions::ActionResult result;
    result.attempted = true;
    const bool copied = std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::none,
        error);
    result.succeeded = copied && !error;
    result.message = result.succeeded
        ? "Copied file to: " + actions::PathToUtf8(destination)
        : "Copy failed: " + error.message();
    return result;
}

actions::ActionResult FileSystemExecutor::MovePath(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision) const
{
    const auto& source = decision.canonicalSource;
    const auto& destination = decision.canonicalDestination;
    std::error_code error;
    if (!std::filesystem::exists(source, error) || error)
    {
        return Failure("Move source does not exist.");
    }
    if (std::filesystem::exists(destination, error))
    {
        return Failure("Move refused because the destination already exists.");
    }

    std::size_t affected = 0;
    if (!IsWithinAffectedEntryLimit(source, affected))
    {
        return Failure("Move affects more than the configured limit of " +
            std::to_string(maxAffectedEntries) + " entries.");
    }
    if (request.dryRun)
    {
        return DryRunSuccess("Would move " + std::to_string(affected) +
            " entries from " + actions::PathToUtf8(source) +
            " to " + actions::PathToUtf8(destination));
    }

    actions::ActionResult result;
    result.attempted = true;
    std::filesystem::rename(source, destination, error);
    result.succeeded = !error;
    result.message = result.succeeded
        ? "Moved path to: " + actions::PathToUtf8(destination)
        : "Move failed without modifying the source: " + error.message();
    return result;
}

actions::ActionResult FileSystemExecutor::MoveToRecycleBin(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision) const
{
    const auto& source = decision.canonicalSource;
    std::error_code error;
    if (!std::filesystem::exists(source, error) || error)
    {
        return Failure("Recycle source does not exist.");
    }

    std::size_t affected = 0;
    if (!IsWithinAffectedEntryLimit(source, affected))
    {
        return Failure("Recycle operation affects more than the configured limit of " +
            std::to_string(maxAffectedEntries) + " entries.");
    }
    if (request.dryRun)
    {
        return DryRunSuccess("Would move " + std::to_string(affected) +
            " entries to the Recycle Bin from " + actions::PathToUtf8(source));
    }

    actions::ActionResult result;
    result.attempted = true;
#ifdef _WIN32
    std::wstring sourceList = source.wstring();
    sourceList.push_back(L'\0');
    sourceList.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = sourceList.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int status = SHFileOperationW(&operation);
    result.succeeded = status == 0 && !operation.fAnyOperationsAborted;
    result.message = result.succeeded
        ? "Moved path to the Recycle Bin: " + actions::PathToUtf8(source)
        : "Recycle Bin operation failed or was aborted (status " + std::to_string(status) + ").";
#else
    result.message = "Recycle Bin execution is only implemented on Windows.";
#endif
    return result;
}

bool FileSystemExecutor::IsWithinAffectedEntryLimit(
    const std::filesystem::path& value,
    std::size_t& outCount) const
{
    outCount = 1;
    std::error_code error;
    if (!std::filesystem::is_directory(value, error) || error)
    {
        return true;
    }

    std::filesystem::recursive_directory_iterator iterator(
        value,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error)
    {
        return false;
    }
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end)
    {
        ++outCount;
        if (outCount > maxAffectedEntries)
        {
            return false;
        }
        iterator.increment(error);
        if (error)
        {
            return false;
        }
    }
    return true;
}

} // namespace revia::filesystem
