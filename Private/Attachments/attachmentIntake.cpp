#include "Attachments/attachmentIntake.h"

#include "Actions/actionTypes.h"
#include "Attachments/documentText.h"
#include "Audit/contentDigest.h"
#include "Core/utf8.h"
#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace revia::attachments
{
namespace
{
std::uint32_t Big16(const std::string_view bytes, const std::size_t at)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 8) | static_cast<unsigned char>(bytes[at + 1]);
}

std::uint32_t Big32(const std::string_view bytes, const std::size_t at)
{
    return (Big16(bytes, at) << 16) | Big16(bytes, at + 2);
}

std::uint32_t Little16(const std::string_view bytes, const std::size_t at)
{
    return static_cast<unsigned char>(bytes[at]) | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8);
}

std::uint32_t Little24(const std::string_view bytes, const std::size_t at)
{
    return Little16(bytes, at) | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16);
}

std::string Lowered(std::string_view text)
{
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return lowered;
}

std::string NewAttachmentId()
{
    std::random_device device;
    std::ostringstream id;
    for (int index = 0; index < 4; ++index)
        id << std::hex << std::setw(8) << std::setfill('0') << device();
    return id.str();
}

std::string StoredExtension(const AttachmentKind kind, const std::string& mimeType)
{
    switch (kind)
    {
        case AttachmentKind::Image:
            return mimeType == "image/jpeg" ? ".jpg" : mimeType == "image/gif" ? ".gif" : ".png";
        case AttachmentKind::Pdf: return ".pdf";
        case AttachmentKind::Docx: return ".docx";
        case AttachmentKind::Text: return ".txt";
        case AttachmentKind::Video: return mimeType == "video/webm" ? ".webm" : ".mp4";
    }
    return ".bin";
}

IntakeResult Refused(std::string message)
{
    IntakeResult result;
    result.status = IntakeStatus::Refused;
    result.message = std::move(message);
    return result;
}

std::uintmax_t LargestLimit(const AttachmentLimits& limits)
{
    return std::max({limits.maxImageBytes, limits.maxTextFileBytes, limits.maxPdfBytes, limits.maxDocxBytes, limits.video.maxBytes});
}
} // namespace

std::string ToString(const AttachmentKind kind)
{
    switch (kind)
    {
        case AttachmentKind::Image: return "image";
        case AttachmentKind::Text: return "text";
        case AttachmentKind::Pdf: return "pdf";
        case AttachmentKind::Docx: return "docx";
        case AttachmentKind::Video: return "video";
    }
    return "unknown";
}

bool IsDocument(const AttachmentKind kind)
{
    return kind == AttachmentKind::Text || kind == AttachmentKind::Pdf || kind == AttachmentKind::Docx;
}

std::string HumanSize(const std::uintmax_t bytes)
{
    std::ostringstream text;
    if (bytes < 1024)
        text << bytes << " B";
    else if (bytes < 1024 * 1024)
        text << (bytes + 512) / 1024 << " KB";
    else
        text << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
    return text.str();
}

std::optional<ImageInfo> InspectImage(const std::string_view bytes)
{
    ImageInfo info;
    if (bytes.size() >= 24 && bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8) && bytes.substr(12, 4) == "IHDR")
    {
        info.mimeType = "image/png";
        info.width = Big32(bytes, 16);
        info.height = Big32(bytes, 20);
    }
    else if (bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8 &&
             static_cast<unsigned char>(bytes[2]) == 0xFF)
    {
        info.mimeType = "image/jpeg";
        std::size_t at = 2;
        while (at + 4 <= bytes.size())
        {
            if (static_cast<unsigned char>(bytes[at]) != 0xFF)
                return std::nullopt;
            const auto marker = static_cast<unsigned char>(bytes[at + 1]);
            if (marker == 0xFF)
            {
                ++at;
                continue;
            }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            {
                at += 2;
                continue;
            }
            const std::uint32_t length = Big16(bytes, at + 2);
            if (length < 2)
                return std::nullopt;
            // Start-of-frame markers carry the size; C4, C8 and CC are other tables.
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
            {
                if (at + 9 > bytes.size())
                    return std::nullopt;
                info.height = Big16(bytes, at + 5);
                info.width = Big16(bytes, at + 7);
                break;
            }
            at += 2 + length;
        }
    }
    else if (bytes.size() >= 10 && (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a"))
    {
        info.mimeType = "image/gif";
        info.width = Little16(bytes, 6);
        info.height = Little16(bytes, 8);
    }
    else if (bytes.size() >= 30 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP")
    {
        info.mimeType = "image/webp";
        const std::string_view chunk = bytes.substr(12, 4);
        if (chunk == "VP8 ")
        {
            info.width = Little16(bytes, 26) & 0x3FFF;
            info.height = Little16(bytes, 28) & 0x3FFF;
        }
        else if (chunk == "VP8L" && static_cast<unsigned char>(bytes[20]) == 0x2F)
        {
            const std::uint32_t bits = Little16(bytes, 21) | (Little16(bytes, 23) << 16);
            info.width = (bits & 0x3FFF) + 1;
            info.height = ((bits >> 14) & 0x3FFF) + 1;
        }
        else if (chunk == "VP8X")
        {
            info.width = Little24(bytes, 24) + 1;
            info.height = Little24(bytes, 27) + 1;
        }
    }
    else
    {
        return std::nullopt;
    }
    if (info.width == 0 || info.height == 0)
        return std::nullopt;
    return info;
}

std::optional<AttachmentKind> DetectKind(const std::string_view bytes, const std::string_view extension, std::string& why)
{
    const std::string ext = Lowered(extension);
    if (InspectImage(bytes))
        return AttachmentKind::Image;
    if ((bytes.size() >= 12 && bytes.substr(4, 4) == "ftyp") ||
        (bytes.size() >= 4 && bytes.substr(0, 4) == std::string_view("\x1A\x45\xDF\xA3", 4)) || ext == ".mp4" || ext == ".webm")
        return AttachmentKind::Video;
    if (bytes.substr(0, 1024).find("%PDF-") != std::string_view::npos)
        return AttachmentKind::Pdf;
    if (bytes.substr(0, 4) == std::string_view("PK\x03\x04", 4))
    {
        if (ext == ".docx" || bytes.find("word/document.xml") != std::string_view::npos)
            return AttachmentKind::Docx;
        why = "ZIP archives and other Office formats are not supported. Attach the files inside it, or save the document as .docx or "
              "PDF.";
        return std::nullopt;
    }
    if (bytes.substr(0, 4) == std::string_view("\xD0\xCF\x11\xE0", 4))
    {
        why = "Old Office files (.doc, .xls, .ppt) are not supported. Save it as .docx or PDF.";
        return std::nullopt;
    }
    if (bytes.substr(0, 2) == "MZ" || bytes.substr(0, 4) == "\x7F" "ELF")
    {
        why = "Programs cannot be attached.";
        return std::nullopt;
    }
    const bool utf16 = bytes.size() >= 2 && ((static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) ||
                                             (static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF));
    if (utf16 || (bytes.find('\0') == std::string_view::npos && revia::utf8::IsValid(bytes)))
        return AttachmentKind::Text;
    why = "This kind of file is not supported. Revia reads images, short MP4/H.264 or WebM/VP8/VP9 clips, PDF, Word (.docx) and UTF-8 text files.";
    return std::nullopt;
}

std::string SanitizeDisplayName(const std::string_view name)
{
    std::string_view base = name;
    if (const std::size_t slash = base.find_last_of("/\\"); slash != std::string_view::npos)
        base.remove_prefix(slash + 1);
    std::string cleaned;
    for (const char character : revia::utf8::Sanitize(base))
    {
        if (static_cast<unsigned char>(character) < 0x20 || character == 0x7F)
            continue;
        cleaned.push_back(character);
    }
    const std::size_t first = cleaned.find_first_not_of(" .");
    cleaned = first == std::string::npos ? std::string() : cleaned.substr(first);
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.'))
        cleaned.pop_back();
    if (cleaned.empty())
        cleaned = "attachment";
    return revia::utf8::Prefix(cleaned, 120);
}

AttachmentIntake::AttachmentIntake(std::filesystem::path filesDirectory, AttachmentLimits inputLimits, VideoDecoderSettings decoderSettings)
    : directory(std::move(filesDirectory)), limits(inputLimits), videoDecoder(std::move(decoderSettings))
{
}

IntakeResult AttachmentIntake::IntakeFile(const std::filesystem::path& source, const std::stop_token stopToken) const
{
    if (stopToken.stop_requested())
        return Refused("Attachment intake was cancelled.");
    std::error_code error;
    const std::string name = SanitizeDisplayName(revia::actions::PathToUtf8(source.filename()));
    const std::filesystem::file_status status = std::filesystem::status(source, error);
    if (error || !std::filesystem::exists(status))
        return Refused("\"" + name + "\" could not be found.");
    if (std::filesystem::is_directory(status))
        return Refused("\"" + name + "\" is a folder. Attach the files inside it instead.");
    if (!std::filesystem::is_regular_file(status))
        return Refused("\"" + name + "\" is not an ordinary file.");
    const std::uintmax_t size = std::filesystem::file_size(source, error);
    if (error)
        return Refused("\"" + name + "\" could not be read.");
    if (size > LargestLimit(limits))
        return Refused("\"" + name + "\" is " + HumanSize(size) + ", larger than the " + HumanSize(LargestLimit(limits)) +
                       " attachment limit.");
    std::ifstream file(source, std::ios::binary);
    if (!file)
        return Refused("\"" + name + "\" could not be opened for reading.");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::uintmax_t>(file.gcount()) != size || file.peek() != std::char_traits<char>::eof())
        return Refused("\"" + name + "\" changed while it was being read. Try again.");
    return IntakeBytes(name, bytes, stopToken);
}

IntakeResult AttachmentIntake::IntakeBytes(const std::string_view rawName, const std::string& bytes, const std::stop_token stopToken) const
{
    if (stopToken.stop_requested())
        return Refused("Attachment intake was cancelled.");
    const std::string name = SanitizeDisplayName(rawName);
    if (bytes.empty())
        return Refused("\"" + name + "\" is empty.");
    if (bytes.size() > LargestLimit(limits))
        return Refused("\"" + name + "\" is larger than the " + HumanSize(LargestLimit(limits)) + " attachment limit.");
    const std::size_t dot = name.find_last_of('.');
    const std::string extension = dot == std::string::npos ? std::string() : name.substr(dot);
    std::string why;
    const std::optional<AttachmentKind> kind = DetectKind(bytes, extension, why);
    if (!kind)
        return Refused("\"" + name + "\" was not attached. " + why);

    StagedAttachment staged;
    staged.displayName = name;
    staged.kind = *kind;
    staged.bytes = bytes.size();
    const auto tooLarge = [&](const std::uintmax_t limit)
    { return Refused("\"" + name + "\" is " + HumanSize(bytes.size()) + "; " + ToString(*kind) + " files may be at most " + HumanSize(limit) + "."); };

    ExtractedText extracted;
    switch (*kind)
    {
        case AttachmentKind::Image:
        {
            if (bytes.size() > limits.maxImageBytes)
                return tooLarge(limits.maxImageBytes);
            const ImageInfo image = *InspectImage(bytes);
            if (image.mimeType == "image/webp")
                return Refused("\"" + name + "\" is a WebP image, which the local vision model cannot read directly. The desktop window "
                               "converts WebP to PNG when Qt's WebP plugin is installed; otherwise save it as PNG or JPEG.");
            if (image.width > limits.maxImageSide || image.height > limits.maxImageSide)
                return Refused("\"" + name + "\" is " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                               " pixels. Images may be at most " + std::to_string(limits.maxImageSide) +
                               " pixels on a side here; the desktop window shrinks larger ones before attaching them.");
            staged.mimeType = image.mimeType;
            staged.width = image.width;
            staged.height = image.height;
            std::string extension = std::filesystem::path(name).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            if (image.mimeType == "image/gif" || extension == ".gif" || extension == ".webp")
                staged.note = "This upload is a still image; animation and later frames are not analyzed.";
            break;
        }
        case AttachmentKind::Text:
            if (bytes.size() > limits.maxTextFileBytes)
                return tooLarge(limits.maxTextFileBytes);
            staged.mimeType = "text/plain";
            extracted = DecodeTextFile(bytes, limits.maxExtractedTextBytes);
            break;
        case AttachmentKind::Pdf:
            if (bytes.size() > limits.maxPdfBytes)
                return tooLarge(limits.maxPdfBytes);
            staged.mimeType = "application/pdf";
            extracted = ExtractPdfText(bytes, limits.maxExtractedTextBytes);
            break;
        case AttachmentKind::Docx:
            if (bytes.size() > limits.maxDocxBytes)
                return tooLarge(limits.maxDocxBytes);
            staged.mimeType = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
            extracted = ExtractDocxText(bytes, limits.maxExtractedTextBytes);
            break;
        case AttachmentKind::Video:
            if (bytes.size() > limits.video.maxBytes)
                return tooLarge(limits.video.maxBytes);
            staged.mimeType = bytes.substr(0, 4) == std::string_view("\x1A\x45\xDF\xA3", 4) ? "video/webm" : "video/mp4";
            break;
    }
    if (IsDocument(*kind))
    {
        if (!extracted.ok)
            return Refused("\"" + name + "\" was not attached. " + extracted.reason);
        if (extracted.text.find_first_not_of(" \n\t") == std::string::npos)
            return Refused("\"" + name + "\" has no text in it.");
        // Discussing passwords is fine; carrying a working credential is not.
        if (const memory::SensitiveFinding finding = memory::DetectSensitiveContent(extracted.text);
            finding && finding.kind != memory::SensitiveClass::NamedSecret)
        {
            IntakeResult withheld;
            withheld.status = IntakeStatus::Withheld;
            withheld.message = "\"" + name + "\" looks like it contains " + finding.description +
                               ", so it was withheld and not attached. Remove the secret and attach it again.";
            return withheld;
        }
        staged.text = std::move(extracted.text);
        staged.pageOffsets = std::move(extracted.pageOffsets);
        staged.textTruncated = extracted.truncated;
        staged.note = extracted.reason;
    }

    staged.id = NewAttachmentId();
    staged.digest = revia::audit::ContentDigest(bytes);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    staged.storedPath = directory / (staged.id + StoredExtension(staged.kind, staged.mimeType));
    const std::filesystem::path partial = directory / (staged.id + ".partial");
    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output)
        {
            std::filesystem::remove(partial, error);
            return Refused("\"" + name + "\" could not be copied into Revia's private attachment folder.");
        }
    }
    std::filesystem::rename(partial, staged.storedPath, error);
    if (error)
    {
        std::filesystem::remove(partial, error);
        return Refused("\"" + name + "\" could not be copied into Revia's private attachment folder.");
    }

    if (staged.kind == AttachmentKind::Video)
    {
        const auto probe = videoDecoder.Probe(staged.storedPath, limits.video, stopToken);
        if (!probe.ok)
        {
            Discard(staged);
            return Refused("\"" + name + "\" was not attached. " + probe.error);
        }
        const auto poster = videoDecoder.Sample(staged.storedPath, probe.info, {0}, limits.video, stopToken);
        if (!poster.ok || poster.frames.empty())
        {
            Discard(staged);
            return Refused("\"" + name + "\" could not be decoded. " + poster.error);
        }
        staged.video = probe.info;
        staged.mimeType = probe.info.mimeType;
        staged.width = probe.info.width;
        staged.height = probe.info.height;
        staged.poster = poster.frames.front().image;
        staged.note = "Video " + FormatVideoTimestamp(probe.info.durationMs) + "; " + probe.info.videoCodec +
                      ". Review uses timestamped frame samples, not continuous video. " +
                      (probe.info.hasAudio ? "The clip has audio; no audio has been analyzed yet." : "No audio stream.");
    }
    if (stopToken.stop_requested())
    {
        Discard(staged);
        return Refused("Attachment intake was cancelled.");
    }

    IntakeResult result;
    result.status = IntakeStatus::Accepted;
    result.message = "Attached \"" + name + "\" (" + HumanSize(staged.bytes) + ").";
    if (!staged.note.empty())
        result.message += " " + staged.note;
    result.attachment = std::move(staged);
    return result;
}

bool AttachmentIntake::Discard(const StagedAttachment& attachment) const
{
    if (attachment.storedPath.empty() || attachment.storedPath.parent_path() != directory)
        return false;
    std::error_code error;
    return std::filesystem::remove(attachment.storedPath, error) && !error;
}

} // namespace revia::attachments
