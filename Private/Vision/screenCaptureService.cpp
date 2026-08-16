#include "Vision/screenCaptureService.h"

#include <chrono>
#include <cstddef>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <gdiplus.h>
#endif

namespace revia::vision
{

namespace
{
#ifdef _WIN32
    bool FindPngEncoder(CLSID& output)
    {
        UINT count = 0;
        UINT bytes = 0;
        if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0)
        {
            return false;
        }
        std::vector<std::byte> storage(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok)
        {
            return false;
        }
        for (UINT index = 0; index < count; ++index)
        {
            if (encoders[index].MimeType != nullptr &&
                std::wstring(encoders[index].MimeType) == L"image/png")
            {
                output = encoders[index].Clsid;
                return true;
            }
        }
        return false;
    }
#endif
}

CaptureResult ScreenCaptureService::CaptureDesktop(
    const std::filesystem::path& outputDirectory) const
{
    CaptureResult result;
    const auto startedAt = std::chrono::steady_clock::now();
#ifdef _WIN32
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);
    if (error)
    {
        result.reason = "The private vision capture directory could not be created.";
        return result;
    }

    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    result.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    result.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (result.width <= 0 || result.height <= 0)
    {
        result.reason = "Windows reported an invalid virtual desktop size.";
        return result;
    }

    HDC screen = GetDC(nullptr);
    HDC memory = screen != nullptr ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = screen != nullptr
        ? CreateCompatibleBitmap(screen, result.width, result.height)
        : nullptr;
    if (screen == nullptr || memory == nullptr || bitmap == nullptr)
    {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory != nullptr) DeleteDC(memory);
        if (screen != nullptr) ReleaseDC(nullptr, screen);
        result.reason = "Windows could not allocate a desktop capture surface.";
        return result;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(
        memory, 0, 0, result.width, result.height,
        screen, left, top, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!copied)
    {
        DeleteObject(bitmap);
        result.reason = "Windows could not copy the virtual desktop image.";
        return result;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok)
    {
        DeleteObject(bitmap);
        result.reason = "Windows PNG encoding could not initialize.";
        return result;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    result.path = outputDirectory / ("screen-" + std::to_string(stamp) + ".png");
    CLSID encoder{};
    bool saved = false;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        saved = FindPngEncoder(encoder) &&
            image.Save(result.path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
    }
    Gdiplus::GdiplusShutdown(token);
    DeleteObject(bitmap);
    if (!saved)
    {
        result.reason = "The desktop capture could not be encoded as PNG.";
        return result;
    }
    result.succeeded = true;
#else
    (void)outputDirectory;
    result.reason = "Desktop capture is currently implemented for Windows.";
#endif
    result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    return result;
}

} // namespace revia::vision
