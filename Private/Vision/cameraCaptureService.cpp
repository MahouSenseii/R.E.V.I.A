#include "Vision/cameraCaptureService.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#endif

namespace revia::vision
{

namespace
{
    double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

#ifdef _WIN32
    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }
        const int length = WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string output(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            output.data(), length, nullptr, nullptr);
        return output;
    }

    // Releases whatever it holds when it leaves scope. Media Foundation hands back a
    // dozen interfaces per capture and an early return on any one of them would leak the
    // rest, which on a camera means the device stays open and the light stays on.
    template <typename Interface>
    class ComPointer
    {
    public:
        ComPointer() = default;
        ~ComPointer() { Reset(); }
        ComPointer(const ComPointer&) = delete;
        ComPointer& operator=(const ComPointer&) = delete;

        Interface** Receive() { Reset(); return &pointer; }
        [[nodiscard]] Interface* Get() const { return pointer; }
        Interface* operator->() const { return pointer; }
        explicit operator bool() const { return pointer != nullptr; }
        void Reset()
        {
            if (pointer != nullptr)
            {
                pointer->Release();
                pointer = nullptr;
            }
        }

    private:
        Interface* pointer = nullptr;
    };

    // Media Foundation is reference counted per process. Starting it per call and
    // shutting it down again keeps this service stateless, which is what lets the camera
    // be closed the moment a frame has been taken.
    class MediaFoundationScope
    {
    public:
        MediaFoundationScope()
            : started(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        {
        }
        ~MediaFoundationScope()
        {
            if (started)
            {
                MFShutdown();
            }
        }
        MediaFoundationScope(const MediaFoundationScope&) = delete;
        MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;
        [[nodiscard]] bool Started() const { return started; }

    private:
        bool started = false;
    };

    // Holds the IMFActivate array Media Foundation allocates with CoTaskMemAlloc.
    class DeviceList
    {
    public:
        ~DeviceList() { Clear(); }
        DeviceList() = default;
        DeviceList(const DeviceList&) = delete;
        DeviceList& operator=(const DeviceList&) = delete;

        void Clear()
        {
            if (devices != nullptr)
            {
                for (UINT32 index = 0; index < count; ++index)
                {
                    if (devices[index] != nullptr)
                    {
                        devices[index]->Release();
                    }
                }
                CoTaskMemFree(devices);
                devices = nullptr;
            }
            count = 0;
        }

        bool Populate()
        {
            Clear();
            ComPointer<IMFAttributes> attributes;
            if (FAILED(MFCreateAttributes(attributes.Receive(), 1)))
            {
                return false;
            }
            if (FAILED(attributes->SetGUID(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
            {
                return false;
            }
            return SUCCEEDED(MFEnumDeviceSources(attributes.Get(), &devices, &count));
        }

        [[nodiscard]] UINT32 Count() const { return count; }
        [[nodiscard]] IMFActivate* At(const UINT32 index) const
        {
            return index < count ? devices[index] : nullptr;
        }

        static std::wstring ReadString(IMFActivate* device, const GUID& key)
        {
            if (device == nullptr)
            {
                return {};
            }
            WCHAR* value = nullptr;
            UINT32 length = 0;
            if (FAILED(device->GetAllocatedString(key, &value, &length)) || value == nullptr)
            {
                return {};
            }
            std::wstring output(value, length);
            CoTaskMemFree(value);
            return output;
        }

    private:
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
    };

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

    // Asks the reader for plain RGB32. The camera may deliver NV12, MJPG, or YUY2
    // natively; setting the output type lets Media Foundation insert its own converter
    // rather than making this file a video codec.
    bool RequestRgb32(IMFSourceReader* reader)
    {
        ComPointer<IMFMediaType> mediaType;
        if (FAILED(MFCreateMediaType(mediaType.Receive())))
        {
            return false;
        }
        if (FAILED(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)))
        {
            return false;
        }
        return SUCCEEDED(reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            nullptr,
            mediaType.Get()));
    }

    bool ReadFrameSize(IMFSourceReader* reader, int& outWidth, int& outHeight)
    {
        ComPointer<IMFMediaType> current;
        if (FAILED(reader->GetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                current.Receive())))
        {
            return false;
        }
        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height)))
        {
            return false;
        }
        outWidth = static_cast<int>(width);
        outHeight = static_cast<int>(height);
        return width > 0 && height > 0;
    }
#endif
}

std::vector<CameraDescriptor> CameraCaptureService::EnumerateCameras() const
{
    std::vector<CameraDescriptor> cameras;
#ifdef _WIN32
    const MediaFoundationScope scope;
    if (!scope.Started())
    {
        return cameras;
    }
    DeviceList devices;
    if (!devices.Populate())
    {
        return cameras;
    }
    for (UINT32 index = 0; index < devices.Count(); ++index)
    {
        CameraDescriptor descriptor;
        descriptor.index = static_cast<int>(index) + 1;
        descriptor.name = WideToUtf8(DeviceList::ReadString(
            devices.At(index), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME));
        descriptor.symbolicLink = WideToUtf8(DeviceList::ReadString(
            devices.At(index),
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK));
        if (descriptor.name.empty())
        {
            descriptor.name = "Camera " + std::to_string(descriptor.index);
        }
        cameras.push_back(std::move(descriptor));
    }
#endif
    return cameras;
}

CameraResolution ResolveCamera(
    const std::vector<CameraDescriptor>& cameras,
    const CameraSelection& selection)
{
    CameraResolution resolution;
    if (cameras.empty())
    {
        resolution.reason = "No camera is attached.";
        return resolution;
    }

    // Identity first, always. The link is the only field that still means the same
    // physical device after something else is plugged in.
    if (!selection.symbolicLink.empty())
    {
        for (const CameraDescriptor& camera : cameras)
        {
            if (camera.symbolicLink == selection.symbolicLink)
            {
                resolution.available = true;
                resolution.index = camera.index;
                resolution.symbolicLink = camera.symbolicLink;
                resolution.name = camera.name;
                resolution.reason = "Using " + camera.name + ".";
                return resolution;
            }
        }
        if (selection.explicitChoice)
        {
            // Deliberately does not fall through to the index below. A chosen camera
            // that is gone is unavailable; substituting whichever device now holds that
            // position is exactly the silent swap this function exists to prevent.
            resolution.reason = "The selected camera is no longer attached. Choose "
                "another camera; Revia will not quietly use a different one.";
            return resolution;
        }
    }

    // No explicit choice: position is an acceptable hint.
    const int wanted = selection.index < 1 ? 1 : selection.index;
    for (const CameraDescriptor& camera : cameras)
    {
        if (camera.index == wanted)
        {
            resolution.available = true;
            resolution.index = camera.index;
            resolution.symbolicLink = camera.symbolicLink;
            resolution.name = camera.name;
            resolution.reason = "Using " + camera.name + ".";
            return resolution;
        }
    }
    resolution.available = true;
    resolution.index = cameras.front().index;
    resolution.symbolicLink = cameras.front().symbolicLink;
    resolution.name = cameras.front().name;
    resolution.reason = "Using " + cameras.front().name + ".";
    return resolution;
}

CameraFrame CameraCaptureService::CaptureFrame(
    const std::filesystem::path& outputDirectory,
    const int cameraIndex,
    const std::string& symbolicLink,
    const int warmupFrames,
    const bool requireSymbolicLink) const
{
    const auto started = std::chrono::steady_clock::now();
    CameraFrame result;

#ifndef _WIN32
    (void)outputDirectory;
    (void)cameraIndex;
    (void)symbolicLink;
    (void)warmupFrames;
    result.reason = "Camera capture is implemented for Windows only.";
    return result;
#else
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);
    if (error)
    {
        result.reason = "The camera output folder could not be created.";
        return result;
    }

    const MediaFoundationScope scope;
    if (!scope.Started())
    {
        result.reason = "Media Foundation could not be started.";
        return result;
    }

    DeviceList devices;
    if (!devices.Populate() || devices.Count() == 0)
    {
        result.reason = "No camera is attached, or the camera is blocked by Windows "
            "privacy settings.";
        return result;
    }

    // The symbolic link wins when it still resolves, because indices renumber whenever a
    // device appears or disappears and a stale index points at the wrong lens rather
    // than at nothing.
    UINT32 chosen = devices.Count();
    if (!symbolicLink.empty())
    {
        for (UINT32 index = 0; index < devices.Count(); ++index)
        {
            if (WideToUtf8(DeviceList::ReadString(
                    devices.At(index),
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)) == symbolicLink)
            {
                chosen = index;
                break;
            }
        }
    }
    if (chosen >= devices.Count())
    {
        // An explicit device that is not here is a refusal, not an invitation to use a
        // different one. Falling back on index would point a lens the user did not
        // choose, which is worse than capturing nothing.
        if (requireSymbolicLink && !symbolicLink.empty())
        {
            result.reason = "The selected camera is no longer attached. Nothing was "
                "captured, because another camera is not the one that was chosen.";
            return result;
        }
        const int wanted = cameraIndex < 1 ? 1 : cameraIndex;
        if (static_cast<UINT32>(wanted) > devices.Count())
        {
            result.reason = "Camera " + std::to_string(wanted) + " is not attached; " +
                std::to_string(devices.Count()) + " camera(s) available.";
            return result;
        }
        chosen = static_cast<UINT32>(wanted) - 1;
    }

    result.deviceName = WideToUtf8(DeviceList::ReadString(
        devices.At(chosen), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME));

    ComPointer<IMFMediaSource> source;
    if (FAILED(devices.At(chosen)->ActivateObject(
            IID_PPV_ARGS(source.Receive()))) || !source)
    {
        result.reason = "The camera could not be opened. Another application may be "
            "using it, or Windows camera access is denied for desktop apps.";
        return result;
    }

    // Advanced video processing is what lets the reader insert a converter. Most webcams
    // deliver NV12, YUY2, or MJPG and never offer RGB32 natively, so without this the
    // format request below is refused and the capture fails on hardware that works
    // perfectly well -- which is exactly what it did before this attribute was set.
    ComPointer<IMFAttributes> readerAttributes;
    if (SUCCEEDED(MFCreateAttributes(readerAttributes.Receive(), 1)))
    {
        readerAttributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    }

    ComPointer<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(
            source.Get(), readerAttributes.Get(), reader.Receive())) || !reader)
    {
        source->Shutdown();
        result.reason = "A reader could not be created for the camera.";
        return result;
    }

    if (!RequestRgb32(reader.Get()))
    {
        source->Shutdown();
        result.reason = "The camera does not offer a format this build can convert.";
        return result;
    }
    if (!ReadFrameSize(reader.Get(), result.width, result.height))
    {
        source->Shutdown();
        result.reason = "The camera did not report a usable frame size.";
        return result;
    }

    // Auto-exposure and auto-white-balance need a few frames to settle. Keeping the
    // first frame reliably produces a black or blown-out image, and describing that
    // costs a full vision round trip to learn nothing.
    const int warmup = warmupFrames < 0 ? 0 : (warmupFrames > 30 ? 30 : warmupFrames);
    ComPointer<IMFSample> sample;
    bool captured = false;
    for (int attempt = 0; attempt <= warmup; ++attempt)
    {
        DWORD streamFlags = 0;
        sample.Reset();
        const HRESULT read = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, nullptr, &streamFlags, nullptr, sample.Receive());
        if (FAILED(read))
        {
            break;
        }
        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            break;
        }
        if (!sample)
        {
            // A null sample with no error is a timing gap, not a failure. Retrying is
            // correct, and it does not count against the warm-up budget.
            --attempt;
            continue;
        }
        if (attempt == warmup)
        {
            captured = true;
            result.warmupFramesDiscarded = warmup;
        }
    }

    if (!captured || !sample)
    {
        source->Shutdown();
        result.reason = "The camera opened but delivered no frame.";
        return result;
    }

    ComPointer<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.Receive())) || !buffer)
    {
        source->Shutdown();
        result.reason = "The captured frame could not be read.";
        return result;
    }

    BYTE* pixels = nullptr;
    DWORD maximumLength = 0;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&pixels, &maximumLength, &currentLength)) || pixels == nullptr)
    {
        source->Shutdown();
        result.reason = "The captured frame could not be locked for reading.";
        return result;
    }

    const int stride = result.width * 4;
    const bool enoughPixels =
        currentLength >= static_cast<DWORD>(stride) * static_cast<DWORD>(result.height);

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput startupInput;
    bool saved = false;
    if (enoughPixels &&
        Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) == Gdiplus::Ok)
    {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        result.path = outputDirectory / ("camera-" + std::to_string(stamp) + ".png");
        {
            // Negative stride with a pointer to the last row: Media Foundation delivers
            // RGB32 bottom-up, and handing GDI+ the buffer as-is saves the frame upside
            // down.
            Gdiplus::Bitmap image(
                result.width,
                result.height,
                -stride,
                PixelFormat32bppRGB,
                pixels + static_cast<std::size_t>(stride) * (result.height - 1));
            CLSID encoder{};
            saved = FindPngEncoder(encoder) &&
                image.Save(result.path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
        }
        Gdiplus::GdiplusShutdown(token);
    }

    buffer->Unlock();
    // Closed as soon as the frame is in hand. The camera light going out is the only
    // part of this a user can actually verify.
    source->Shutdown();

    if (!saved)
    {
        result.path.clear();
        result.reason = enoughPixels
            ? "The frame was captured but could not be written to disk."
            : "The camera returned a short frame buffer.";
        return result;
    }

    result.succeeded = true;
    result.reason = "Captured one frame from " +
        (result.deviceName.empty() ? std::string("the camera") : result.deviceName) + ".";
    result.elapsedMilliseconds = ElapsedMilliseconds(started);
    return result;
#endif
}

} // namespace revia::vision
