#pragma once

#include "Attachments/videoDecoder.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace revia::attachments
{

enum class AttachmentKind
{
    Image,
    Text,
    Pdf,
    Docx,
    Video
};

[[nodiscard]] std::string ToString(AttachmentKind kind);
[[nodiscard]] bool IsDocument(AttachmentKind kind);

// What a file the user hands over may be. The desktop shrinks images to at most
// DesktopImageSide pixels on the long side and re-encodes them as PNG before they get
// here, so the image limits below only bound what reaches the core by other routes.
struct AttachmentLimits
{
    std::uintmax_t maxImageBytes = 8 * 1024 * 1024;
    std::uint32_t maxImageSide = 2048;
    std::uintmax_t maxTextFileBytes = 2 * 1024 * 1024;
    std::uintmax_t maxPdfBytes = 20 * 1024 * 1024;
    std::uintmax_t maxDocxBytes = 20 * 1024 * 1024;
    // Text kept from one document after extraction.
    std::size_t maxExtractedTextBytes = 4 * 1024 * 1024;
    // A document whose text is at most this long goes into the turn whole; a longer one
    // is chunked, indexed, and only its most relevant passages are shown.
    std::size_t inlineTextBytes = 6000;
    std::size_t maxAttachmentsPerTurn = 8;
    std::size_t maxImagesPerTurn = 4;
    VideoLimits video;
};

inline constexpr std::uint32_t DesktopImageSide = 1024;

enum class IntakeStatus
{
    Accepted,
    // Read, but kept out because its text looks like it carries a secret.
    Withheld,
    Refused
};

// A file accepted and copied into the companion's private store, waiting to be sent
// with a message. Nothing here is the original path: once copied, the user's file is
// never read again.
struct StagedAttachment
{
    std::string id;
    std::string displayName;
    AttachmentKind kind = AttachmentKind::Text;
    std::string mimeType;
    std::uintmax_t bytes = 0;
    // SHA-256 of the exact bytes stored.
    std::string digest;
    std::filesystem::path storedPath;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VideoInfo video;
    std::optional<conversationImage> poster;
    // Documents only: the extracted UTF-8 text and where each page starts in it.
    std::string text;
    std::vector<std::size_t> pageOffsets;
    bool textTruncated = false;
    // Anything the user should know about what was read, such as text left out.
    std::string note;
};

struct IntakeResult
{
    IntakeStatus status = IntakeStatus::Refused;
    // What happened, in words for the user. Never any of the file's content.
    std::string message;
    StagedAttachment attachment;
};

[[nodiscard]] std::string HumanSize(std::uintmax_t bytes);

} // namespace revia::attachments
