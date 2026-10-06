#include "Visual/imageArtifact.h"
#include "Audit/contentDigest.h"

#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#include <gdiplus.h>
#endif

namespace revia::visual
{
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
    const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad() || bytes.size() != size || audit::ContentDigest(bytes) != receipt.sha256)
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
    {
        Gdiplus::Bitmap bitmap(expected.c_str());
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
