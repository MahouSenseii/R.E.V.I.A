#include "Vision/screenCaptureService.h"

#include <algorithm>
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
    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }
        const int count = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, nullptr);
        if (count <= 0)
        {
            return {};
        }
        std::string output(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            output.data(), count, nullptr, nullptr);
        return output;
    }

    void ReadWindowIdentity(const HWND window, CaptureResult& result)
    {
        if (window == nullptr)
        {
            return;
        }
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (process != nullptr)
        {
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &length))
            {
                path.resize(length);
                result.foregroundApplication = WideToUtf8(
                    std::filesystem::path(path).filename().wstring());
            }
            CloseHandle(process);
        }
        const int titleLength = GetWindowTextLengthW(window);
        if (titleLength > 0)
        {
            std::wstring title(static_cast<std::size_t>(titleLength + 1), L'\0');
            const int copied = GetWindowTextW(window, title.data(), titleLength + 1);
            if (copied > 0)
            {
                title.resize(static_cast<std::size_t>(copied));
                result.foregroundWindowTitle = WideToUtf8(title);
            }
        }
    }

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

    bool CaptureRegion(
        const std::filesystem::path& outputDirectory,
        CaptureResult& result)
    {
        std::error_code error;
        std::filesystem::create_directories(outputDirectory, error);
        if (error)
        {
            result.reason = "The private vision capture directory could not be created.";
            return false;
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
            result.reason = "Windows could not allocate a screen capture surface.";
            return false;
        }
        HGDIOBJ previous = SelectObject(memory, bitmap);
        const BOOL copied = BitBlt(
            memory, 0, 0, result.width, result.height,
            screen, result.originX, result.originY, SRCCOPY | CAPTUREBLT);
        SelectObject(memory, previous);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        if (!copied)
        {
            DeleteObject(bitmap);
            result.reason = "Windows could not copy the requested screen region.";
            return false;
        }

        Gdiplus::GdiplusStartupInput startupInput;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok)
        {
            DeleteObject(bitmap);
            result.reason = "Windows PNG encoding could not initialize.";
            return false;
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
            result.reason = "The screen capture could not be encoded as PNG.";
            return false;
        }
        result.succeeded = true;
        return true;
    }
#endif
}

CaptureResult ScreenCaptureService::CaptureDesktop(
    const std::filesystem::path& outputDirectory) const
{
    CaptureResult result;
    const auto startedAt = std::chrono::steady_clock::now();
#ifdef _WIN32
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    result.originX = left;
    result.originY = top;
    result.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    result.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (result.width <= 0 || result.height <= 0)
    {
        result.reason = "Windows reported an invalid virtual desktop size.";
        return result;
    }

    ReadWindowIdentity(GetForegroundWindow(), result);
    CaptureRegion(outputDirectory, result);
#else
    (void)outputDirectory;
    result.reason = "Desktop capture is currently implemented for Windows.";
#endif
    result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    return result;
}

CaptureResult ScreenCaptureService::CaptureForegroundWindow(
    const std::filesystem::path& outputDirectory) const
{
    CaptureResult result;
    const auto startedAt = std::chrono::steady_clock::now();
#ifdef _WIN32
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || !IsWindowVisible(foreground) || IsIconic(foreground))
    {
        result.reason = "Windows did not report a visible foreground window.";
    }
    else
    {
        RECT bounds{};
        if (!GetWindowRect(foreground, &bounds))
        {
            result.reason = "Windows could not read the foreground window bounds.";
        }
        else
        {
            const int desktopLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int desktopTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int desktopRight = desktopLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int desktopBottom = desktopTop + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            result.originX = std::max(static_cast<int>(bounds.left), desktopLeft);
            result.originY = std::max(static_cast<int>(bounds.top), desktopTop);
            const int right = std::min(static_cast<int>(bounds.right), desktopRight);
            const int bottom = std::min(static_cast<int>(bounds.bottom), desktopBottom);
            result.width = right - result.originX;
            result.height = bottom - result.originY;
            if (result.width <= 0 || result.height <= 0)
            {
                result.reason = "The foreground window is outside the visible desktop.";
            }
            else
            {
                ReadWindowIdentity(foreground, result);
                CaptureRegion(outputDirectory, result);
                if (result.succeeded && GetForegroundWindow() != foreground)
                {
                    std::error_code cleanupError;
                    std::filesystem::remove(result.path, cleanupError);
                    result.succeeded = false;
                    result.path.clear();
                    result.reason = "The foreground window changed during capture.";
                }
            }
        }
    }
#else
    (void)outputDirectory;
    result.reason = "Foreground-window capture is currently implemented for Windows.";
#endif
    result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    return result;
}

} // namespace revia::vision
