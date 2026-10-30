#include "Visual/imageArtifact.h"
#include "Audit/contentDigest.h"

#include <fstream>
#include <iterator>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#endif

namespace revia::visual
{
namespace
{
bool IsDigest(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char character)
                                     { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
}

bool ContainsAnimation(const std::string& bytes)
{
    if (!bytes.starts_with("\x89PNG\r\n\x1a\n"))
        return false;
    std::size_t offset = 8;
    while (offset + 12 <= bytes.size())
    {
        std::uint32_t length = 0;
        for (int index = 0; index < 4; ++index)
            length = (length << 8) | static_cast<unsigned char>(bytes[offset + index]);
        if (length > bytes.size() - offset - 12)
            return true;
        if (bytes.compare(offset + 4, 4, "acTL") == 0)
            return true;
        offset += 12 + length;
    }
    return false;
}
}

bool VerifyImageSource(const ImageSource& source, std::string& outError)
{
    outError = "The reference requires bounded, static PNG or JPEG bytes with matching identity, digest and dimensions.";
    if (source.id.empty() || source.id.size() > 128 || source.id.find('\0') != std::string::npos || !IsDigest(source.digest) ||
        !source.bytes || source.bytes->empty() || source.bytes->size() > 8 * 1024 * 1024 || source.width == 0 || source.height == 0 ||
        source.width > 2048 || source.height > 2048 || (source.mimeType != "image/png" && source.mimeType != "image/jpeg") ||
        audit::ContentDigest(*source.bytes) != source.digest || ContainsAnimation(*source.bytes))
        return false;
#ifdef _WIN32
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return false;
    bool valid = false;
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(source.bytes->data()), static_cast<UINT>(source.bytes->size()));
    if (stream)
    {
        {
            Gdiplus::Bitmap bitmap(stream);
            GUID format{};
            const auto& expectedFormat = source.mimeType == "image/png" ? Gdiplus::ImageFormatPNG : Gdiplus::ImageFormatJPEG;
            if (bitmap.GetLastStatus() == Gdiplus::Ok && bitmap.GetWidth() == source.width && bitmap.GetHeight() == source.height &&
                bitmap.GetRawFormat(&format) == Gdiplus::Ok && IsEqualGUID(format, expectedFormat))
            {
                Gdiplus::Rect bounds(0, 0, source.width, source.height);
                Gdiplus::BitmapData pixels{};
                valid = bitmap.LockBits(&bounds, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &pixels) == Gdiplus::Ok;
                if (valid)
                {
                    valid = pixels.Scan0 != nullptr;
                    bitmap.UnlockBits(&pixels);
                }
            }
        }
        stream->Release();
    }
    Gdiplus::GdiplusShutdown(token);
    if (valid)
        outError.clear();
    return valid;
#else
    outError = "Native reference decoding is currently available on Windows.";
    return false;
#endif
}

bool VerifyImageArtifact(const ImageArtifactExpectation& expected, const ImageArtifactReceipt& receipt, std::string& outError)
{
    outError = "The generated artifact's mode, source or effective generation parameters do not match this request.";
    if (expected.provider != "local_diffusers" || receipt.provider != expected.provider || receipt.mode != expected.mode ||
        receipt.pipeline.empty() || receipt.pipeline.size() > 128 ||
        (!expected.pipeline.empty() && receipt.pipeline != expected.pipeline) || receipt.variant != expected.variant ||
        receipt.seed != expected.seed || receipt.requestedSteps != expected.requestedSteps ||
        receipt.totalInferenceSteps != expected.totalInferenceSteps || receipt.denoisingSteps != expected.denoisingSteps ||
        expected.requestedSteps < 1 || expected.requestedSteps > 50 || expected.totalInferenceSteps < 1 ||
        expected.totalInferenceSteps > 50 || expected.denoisingSteps < 1 || expected.denoisingSteps > expected.totalInferenceSteps ||
        !std::isfinite(receipt.requestedStrength) || !std::isfinite(receipt.effectiveStrength) ||
        receipt.requestedStrength != expected.requestedStrength || receipt.effectiveStrength != expected.effectiveStrength ||
        receipt.sourceId != expected.sourceId || receipt.sourceDigest != expected.sourceDigest ||
        receipt.sourcePreparation != expected.sourcePreparation)
        return false;
    if (expected.mode == ImageRequestMode::ReferenceToImage)
    {
        if (!std::isfinite(expected.requestedStrength) || expected.requestedStrength <= 0 || expected.requestedStrength >= 1 ||
            expected.effectiveStrength != expected.requestedStrength || expected.sourceId.empty() || expected.sourceId.size() > 128 ||
            !IsDigest(expected.sourceDigest) || !IsDigest(receipt.preparedSourceDigest) ||
            expected.sourcePreparation != "contain-pad-rgb-v1" ||
            expected.denoisingSteps != static_cast<int>(expected.totalInferenceSteps * expected.effectiveStrength))
            return false;
    }
    else if (expected.mode != ImageRequestMode::TextToImage || expected.requestedStrength != 0 || expected.effectiveStrength != 0 ||
             !expected.sourceId.empty() || !expected.sourceDigest.empty() || !expected.sourcePreparation.empty() ||
             !receipt.preparedSourceDigest.empty() || expected.denoisingSteps != expected.totalInferenceSteps)
        return false;
    std::error_code error;
    if (expected.path.empty() || expected.jobId.empty() || expected.model.empty() || std::filesystem::is_symlink(expected.path, error) ||
        error)
        return false;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(expected.path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
#endif
    return VerifyImageArtifact(expected.path, expected.jobId, expected.model, expected.width, expected.height, receipt, outError);
}

bool VerifyImageArtifact(const std::filesystem::path& expectedPath, const std::string& jobId, const std::string& model, const int width,
    const int height, const ImageArtifactReceipt& receipt, std::string& outError)
{
    outError = "The generated artifact's ownership or provenance does not match this request.";
    std::error_code error;
    const auto expected = std::filesystem::weakly_canonical(expectedPath, error);
    if (error || receipt.jobId != jobId || receipt.model != model || receipt.width != width || receipt.height != height || width <= 0 ||
        height <= 0)
    {
        return false;
    }
    const auto received = std::filesystem::weakly_canonical(receipt.path, error);
    if (error || received != expected || !std::filesystem::is_regular_file(expected, error) || error)
    {
        return false;
    }
    const auto size = std::filesystem::file_size(expected, error);
    if (error || size == 0 || size > 32 * 1024 * 1024)
    {
        outError = "The generated artifact is empty or exceeds the image size limit.";
        return false;
    }
    std::ifstream stream(expected, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size) + 1, '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    if (stream.bad() || bytes.size() != size || audit::ContentDigest(bytes) != receipt.sha256 || ContainsAnimation(bytes))
    {
        outError = "The generated artifact's contents do not match its receipt.";
        return false;
    }
    outError = "The generated artifact could not be decoded at the requested dimensions.";
#ifdef _WIN32
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
    {
        return false;
    }
    bool valid = false;
    IStream* encoded = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
    if (encoded)
    {
        {
            Gdiplus::Bitmap bitmap(encoded);
            GUID format{};
            if (bitmap.GetLastStatus() == Gdiplus::Ok && bitmap.GetWidth() == static_cast<UINT>(width) &&
                bitmap.GetHeight() == static_cast<UINT>(height) && bitmap.GetRawFormat(&format) == Gdiplus::Ok &&
                IsEqualGUID(format, Gdiplus::ImageFormatPNG))
            {
                Gdiplus::Rect bounds(0, 0, width, height);
                Gdiplus::BitmapData pixels{};
                valid = bitmap.LockBits(&bounds, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &pixels) == Gdiplus::Ok;
                if (valid)
                {
                    valid = pixels.Scan0 != nullptr;
                    bitmap.UnlockBits(&pixels);
                }
            }
        }
        encoded->Release();
    }
    Gdiplus::GdiplusShutdown(token);
    if (valid)
    {
        outError.clear();
    }
    return valid;
#else
    outError = "Native image decoding is currently available on Windows.";
    return false;
#endif
}
}
