#include "Attachments/zipArchive.h"

#include "Attachments/inflate.h"

#include <algorithm>
#include <cstdint>

namespace revia::attachments
{
namespace
{
constexpr std::uint32_t EndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t CentralDirectoryEntry = 0x02014b50;
constexpr std::uint32_t LocalFileHeader = 0x04034b50;
constexpr std::size_t MaximumEntries = 10000;

std::uint32_t Read16(const std::string_view bytes, const std::size_t at)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8);
}

std::uint32_t Read32(const std::string_view bytes, const std::size_t at)
{
    return Read16(bytes, at) | (Read16(bytes, at + 2) << 16);
}

ZipEntry Failed(std::string why, const bool found = false)
{
    ZipEntry entry;
    entry.found = found;
    entry.error = std::move(why);
    return entry;
}
} // namespace

ZipEntry ReadZipEntry(const std::string_view archive, const std::string_view name, const std::size_t maxBytes)
{
    if (archive.size() < 22)
        return Failed("The file is too short to be a ZIP archive.");
    // The end record sits in the last 22 bytes plus an optional comment of up to 64 KiB.
    const std::size_t earliest = archive.size() > 22 + 0xffff ? archive.size() - 22 - 0xffff : 0;
    std::size_t end = std::string_view::npos;
    for (std::size_t at = archive.size() - 22 + 1; at-- > earliest;)
    {
        if (Read32(archive, at) == EndOfCentralDirectory)
        {
            end = at;
            break;
        }
    }
    if (end == std::string_view::npos)
        return Failed("No ZIP directory was found.");
    const std::uint32_t entries = Read16(archive, end + 10);
    const std::uint32_t directorySize = Read32(archive, end + 12);
    const std::uint32_t directoryOffset = Read32(archive, end + 16);
    if (entries == 0xffff || directorySize == 0xffffffffU || directoryOffset == 0xffffffffU)
        return Failed("ZIP64 archives are not supported.");
    if (entries > MaximumEntries)
        return Failed("The archive has too many members.");
    if (static_cast<std::uint64_t>(directoryOffset) + directorySize > end)
        return Failed("The ZIP directory points outside the file.");

    std::size_t cursor = directoryOffset;
    for (std::uint32_t index = 0; index < entries; ++index)
    {
        if (cursor + 46 > end || Read32(archive, cursor) != CentralDirectoryEntry)
            return Failed("The ZIP directory is damaged.");
        const std::uint32_t flags = Read16(archive, cursor + 8);
        const std::uint32_t method = Read16(archive, cursor + 10);
        const std::uint32_t compressedSize = Read32(archive, cursor + 20);
        const std::uint32_t uncompressedSize = Read32(archive, cursor + 24);
        const std::uint32_t nameLength = Read16(archive, cursor + 28);
        const std::uint32_t extraLength = Read16(archive, cursor + 30);
        const std::uint32_t commentLength = Read16(archive, cursor + 32);
        const std::uint32_t localOffset = Read32(archive, cursor + 42);
        if (cursor + 46 + nameLength + extraLength + commentLength > end)
            return Failed("The ZIP directory is damaged.");
        const std::string_view entryName = archive.substr(cursor + 46, nameLength);
        cursor += 46 + nameLength + extraLength + commentLength;
        if (entryName != name)
            continue;

        if ((flags & 0x1U) != 0)
            return Failed("The document is encrypted.", true);
        if (method != 0 && method != 8)
            return Failed("The document uses an unsupported compression method.", true);
        if (uncompressedSize > maxBytes)
            return Failed("The document's text part is larger than the limit.", true);
        if (static_cast<std::uint64_t>(localOffset) + 30 > archive.size() || Read32(archive, localOffset) != LocalFileHeader)
            return Failed("The ZIP member header is damaged.", true);
        const std::uint64_t dataStart =
            static_cast<std::uint64_t>(localOffset) + 30 + Read16(archive, localOffset + 26) + Read16(archive, localOffset + 28);
        if (dataStart + compressedSize > archive.size())
            return Failed("The ZIP member runs past the end of the file.", true);
        const std::string_view compressed = archive.substr(static_cast<std::size_t>(dataStart), compressedSize);

        ZipEntry entry;
        entry.found = true;
        if (method == 0)
        {
            if (compressedSize != uncompressedSize)
                return Failed("The ZIP member sizes disagree.", true);
            entry.data.assign(compressed);
        }
        else
        {
            InflateResult inflated = InflateRaw(compressed, uncompressedSize);
            if (inflated.status != InflateStatus::Complete || inflated.output.size() != uncompressedSize)
                return Failed("The ZIP member could not be decompressed.", true);
            entry.data = std::move(inflated.output);
        }
        entry.ok = true;
        return entry;
    }
    return Failed("The archive has no " + std::string(name) + ".");
}

} // namespace revia::attachments
