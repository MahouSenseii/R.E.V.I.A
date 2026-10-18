#pragma once

#include "Attachments/attachmentTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace revia::attachments
{

struct ImageInfo
{
    std::string mimeType;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Image type and size from the header alone: PNG, JPEG, GIF and WebP. Nothing is
// decoded. Empty for anything else or a header that does not hold together.
[[nodiscard]] std::optional<ImageInfo> InspectImage(std::string_view bytes);

// The file's kind by its content first and its extension second. Empty when it is
// none of the supported kinds; `why` then says what it looked like.
[[nodiscard]] std::optional<AttachmentKind> DetectKind(std::string_view bytes, std::string_view extension, std::string& why);

// A name safe to show and store: the last path component only, no control characters,
// valid UTF-8, bounded length.
[[nodiscard]] std::string SanitizeDisplayName(std::string_view name);

// The one way a file the user chose enters Revia. It is read once, bounded, checked by
// content, its text extracted and screened for secrets, and the bytes are copied into
// this companion's private attachment store under a random name. The file is never
// opened with its registered program, executed, or read from its original path again.
// This is deliberately separate from the approved file roots: handing a file over is a
// one-off act by the user, not access to a folder.
class AttachmentIntake
{
public:
    explicit AttachmentIntake(std::filesystem::path filesDirectory, AttachmentLimits limits = {});

    [[nodiscard]] IntakeResult IntakeFile(const std::filesystem::path& source) const;
    // For content with no file behind it, such as an image pasted from the clipboard.
    [[nodiscard]] IntakeResult IntakeBytes(std::string_view name, const std::string& bytes) const;
    // Removes a staged copy that was never sent.
    bool Discard(const StagedAttachment& attachment) const;

    [[nodiscard]] const AttachmentLimits& Limits() const { return limits; }
    [[nodiscard]] const std::filesystem::path& Directory() const { return directory; }

private:
    std::filesystem::path directory;
    AttachmentLimits limits;
};

} // namespace revia::attachments
