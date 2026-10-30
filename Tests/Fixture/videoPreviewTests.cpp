#include "desktopSuites.h"
#include "videoPreviewDialog.h"
#include "Core/runtimePath.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using revia::attachments::VideoPlaybackSource;

struct Results
{
    int failures = 0;

    void Expect(const bool condition, const char* message)
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    }
};

bool WaitUntil(const std::function<bool()>& ready, const int milliseconds = 1500)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < milliseconds)
    {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
    QCoreApplication::processEvents();
    return ready();
}

void TestCloseCancelsOffThreadSourceValidation(Results& result)
{
    const auto uiThread = std::this_thread::get_id();
    std::atomic<bool> entered{false}, offThread{false}, cancelled{false}, wrongAdmissionThread{false};
    VideoPreviewDialog dialog(
        "Validation cancellation",
        [&](const std::stop_token token) -> std::optional<VideoPlaybackSource>
        {
            offThread = std::this_thread::get_id() != uiThread;
            entered = true;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(2ms);
            cancelled = token.stop_requested();
            return std::nullopt;
        },
        [&]
        {
            if (std::this_thread::get_id() != uiThread)
                wrongAdmissionThread = true;
            return true;
        });
    dialog.show();
    result.Expect(WaitUntil([&] { return entered.load(); }), "Video source validation never began.");
    result.Expect(offThread, "The video source provider blocked the UI thread.");
    bool uiTick = false;
    QTimer::singleShot(0, [&] { uiTick = true; });
    result.Expect(WaitUntil([&] { return uiTick; }, 100), "The UI could not process a callback during source validation.");
    QElapsedTimer closeTime;
    closeTime.start();
    dialog.close();
    result.Expect(closeTime.elapsed() < 1000 && cancelled, "Closing did not cancel and join source validation promptly.");
    result.Expect(!dialog.IsRunning() && dialog.QueuedFrameCount() == 0, "Closed preview retained work or queued pixels.");
    result.Expect(!wrongAdmissionThread, "Video admission ran outside the UI thread.");
}

void TestScopeWithdrawalClosesPendingPreview(Results& result)
{
    std::atomic<bool> entered{false}, allowed{true}, cancelled{false};
    VideoPreviewDialog dialog(
        "Scope retirement",
        [&](const std::stop_token token) -> std::optional<VideoPlaybackSource>
        {
            entered = true;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(2ms);
            cancelled = token.stop_requested();
            return std::nullopt;
        },
        [&] { return allowed.load(); });
    dialog.show();
    result.Expect(WaitUntil([&] { return entered.load(); }), "The pending preview did not start validation.");
    allowed = false;
    result.Expect(WaitUntil([&] { return !dialog.isVisible() && !dialog.IsRunning(); }),
        "A retired media/session scope left its preview open or active.");
    result.Expect(cancelled && dialog.QueuedFrameCount() == 0, "Scope retirement did not discard pending preview work.");
}

void TestMissingAndThrowingAdmissionNeverReadSource(Results& result)
{
    for (const bool throwing : {false, true})
    {
        std::atomic<int> reads{0};
        VideoPreviewDialog dialog(
            "Unavailable scope",
            [&](std::stop_token) -> std::optional<VideoPlaybackSource>
            {
                ++reads;
                return std::nullopt;
            },
            [throwing]() -> bool
            {
                if (throwing)
                    throw std::runtime_error("Retired owner");
                return false;
            });
        dialog.show();
        result.Expect(WaitUntil([&] { return !dialog.isVisible(); }), "Unavailable video admission did not close the preview.");
        result.Expect(reads == 0 && !dialog.IsRunning(), "Unavailable admission still read private video data.");
    }
}

QString CpuH264Encoder(Results& result)
{
    const auto ffmpeg = revia::core::ResolveRuntimePath(std::filesystem::path("ThirdParty/ffmpeg/bin/ffmpeg.exe"));
    QProcess inspect;
    inspect.setProgram(QString::fromStdWString(ffmpeg.wstring()));
    inspect.setArguments({"-hide_banner", "-encoders"});
    inspect.start();
    const bool inspected = inspect.waitForFinished(15000) && inspect.exitStatus() == QProcess::NormalExit && inspect.exitCode() == 0;
    if (!inspected)
    {
        inspect.kill();
        inspect.waitForFinished(1000);
        result.Expect(false, "The configured FFmpeg could not report its fixture encoders.");
        return {};
    }
    const auto encoders = inspect.readAllStandardOutput().toStdString();
    for (const char* name : {"libopenh264", "libx264"})
        if (std::regex_search(encoders, std::regex(std::string("\\b") + name + "\\b")))
            return QString::fromLatin1(name);
    return {};
}

void TestRealPreviewPlaybackSeekAndRetirement(Results& result, const QString& h264)
{
    QTemporaryDir runtime;
    result.Expect(runtime.isValid(), "A disposable video preview directory could not be created.");
    if (!runtime.isValid())
        return;
    const QString previous = QDir::currentPath();
    struct RestoreDirectory
    {
        QString path;
        ~RestoreDirectory()
        {
            QDir::setCurrent(path);
        }
    } restore{previous};
    result.Expect(QDir().mkpath(runtime.path() + "/Config") && QDir::setCurrent(runtime.path()),
        "The isolated preview runtime could not be selected.");
    const auto ffmpeg = revia::core::ResolveRuntimePath(std::filesystem::path("ThirdParty/ffmpeg/bin/ffmpeg.exe"));
    const QString clip = runtime.path() + "/two-scenes.mp4";
    QProcess create;
    create.setProgram(QString::fromStdWString(ffmpeg.wstring()));
    create.setArguments({"-nostdin", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=red:s=160x96:r=15:d=2", "-f",
        "lavfi", "-i", "color=c=blue:s=160x96:r=15:d=2", "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-c:v", h264,
        "-pix_fmt", "yuv420p", "-threads", "1", "-y", clip});
    create.start();
    const bool created = create.waitForFinished(15000) && create.exitStatus() == QProcess::NormalExit && create.exitCode() == 0;
    if (!created)
    {
        create.kill();
        create.waitForFinished(1000);
        std::cerr << "Video fixture tool: " << ffmpeg.string() << "; " << create.errorString().toStdString() << "; "
                  << create.readAllStandardError().toStdString() << '\n';
        result.Expect(false, "The installed FFmpeg could not create the real red/blue preview fixture.");
        return;
    }
    const revia::attachments::VideoDecoder decoder;
    const auto nativeClip = std::filesystem::path(clip.toStdWString());
    const auto probe = decoder.Probe(nativeClip);
    result.Expect(probe.ok, "Default video tools were unavailable from an isolated runtime with its own Config.");
    if (!probe.ok)
        return;
    VideoPlaybackSource source;
    source.path = nativeClip;
    source.info = probe.info;
    source.reference.id = "preview-source";
    std::atomic<bool> allowed{true}, available{true}, wrongThread{false};
    std::atomic<int> reads{0};
    const auto uiThread = std::this_thread::get_id();
    VideoPreviewDialog dialog(
        "Two actual scenes",
        [&](const std::stop_token token) -> std::optional<VideoPlaybackSource>
        {
            if (std::this_thread::get_id() == uiThread)
                wrongThread = true;
            ++reads;
            return !token.stop_requested() && available ? std::optional(source) : std::nullopt;
        },
        [&]
        {
            if (std::this_thread::get_id() != uiThread)
                wrongThread = true;
            return allowed.load();
        });
    dialog.show();
    auto* frame = dialog.findChild<QLabel*>("videoPreviewFrame");
    auto* play = dialog.findChild<QPushButton*>("videoPlayButton");
    auto* pause = dialog.findChild<QPushButton*>("videoPauseButton");
    auto* slider = dialog.findChild<QSlider*>("videoSeekSlider");
    result.Expect(frame && play && pause && slider, "The preview playback controls are missing.");
    if (!frame || !play || !pause || !slider)
        return;
    result.Expect(WaitUntil([&] { return dialog.CurrentTimestampMs() >= 0 && !dialog.IsRunning(); }, 5000),
        "The real video poster was not decoded and paused.");
    const auto center = [&]
    {
        const QImage image = frame->pixmap().toImage();
        return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
    };
    result.Expect(
        !dialog.IsPlaying() && center().red() > 180 && center().blue() < 70, "The initial poster was not the actual first red scene.");
    std::size_t maxQueued = 0;
    play->click();
    result.Expect(WaitUntil(
                      [&]
                      {
                          maxQueued = std::max(maxQueued, dialog.QueuedFrameCount());
                          return dialog.CurrentTimestampMs() >= 400;
                      },
                      5000),
        "Play did not advance actual decoded video timestamps.");
    pause->click();
    const auto pausedAt = dialog.CurrentTimestampMs();
    QElapsedTimer paused;
    paused.start();
    WaitUntil([&] { return paused.elapsed() >= 250; }, 500);
    result.Expect(!dialog.IsRunning() && !dialog.IsPlaying() && dialog.QueuedFrameCount() == 0 && dialog.CurrentTimestampMs() == pausedAt,
        "Pause left a decoder running or advanced the displayed timestamp.");
    bool rewound = false;
    play->click();
    result.Expect(WaitUntil(
                      [&]
                      {
                          const auto current = dialog.CurrentTimestampMs();
                          rewound |= current >= 0 && current < pausedAt;
                          maxQueued = std::max(maxQueued, dialog.QueuedFrameCount());
                          return current > pausedAt + 60;
                      },
                      5000) &&
                      !rewound,
        "Resume restarted at an older frame instead of the last actual displayed time.");
    pause->click();
    const int beforeSeek = reads;
    slider->setValue(2500);
    result.Expect(WaitUntil([&] { return dialog.CurrentTimestampMs() >= 2500 && !dialog.IsRunning(); }, 5000),
        "Seeking did not decode a new timestamp while paused.");
    result.Expect(reads > beforeSeek && center().blue() > 180 && center().red() < 70,
        "Seek reused the red poster instead of revalidating and decoding the actual blue scene.");
    available = false;
    slider->setValue(1000);
    result.Expect(WaitUntil([&] { return dialog.CurrentTimestampMs() < 0 && !dialog.IsRunning() && frame->pixmap().isNull(); }, 3000),
        "A failed source revalidation kept old pixels visible.");
    available = true;
    play->click();
    result.Expect(WaitUntil([&] { return dialog.CurrentTimestampMs() >= 200 && dialog.IsPlaying(); }, 5000),
        "The preview could not start after fresh successful source validation.");
    allowed = false;
    result.Expect(WaitUntil([&] { return !dialog.isVisible() && !dialog.IsRunning(); }),
        "Scope loss during real playback left the preview open or its decoder active.");
    result.Expect(dialog.QueuedFrameCount() == 0 && frame->pixmap().isNull() && !wrongThread && maxQueued <= 3,
        "Playback retirement leaked pixels, exceeded the three-frame bound or crossed the UI admission boundary.");
}

int RunVideoPreviewTests(int, char**)
{
    Results result;
    TestCloseCancelsOffThreadSourceValidation(result);
    TestScopeWithdrawalClosesPendingPreview(result);
    TestMissingAndThrowingAdmissionNeverReadSource(result);
    if (result.failures != 0)
        return 1;
    const auto availability = revia::attachments::VideoDecoder().Availability();
    if (!availability.empty())
    {
        std::cout << "SKIP: Preview lifetime/admission checks passed; actual playback requires local FFmpeg/ffprobe. " << availability
                  << '\n';
        return 77;
    }
    const auto h264 = CpuH264Encoder(result);
    if (result.failures != 0)
        return 1;
    if (h264.isEmpty())
    {
        std::cout << "SKIP: Preview lifetime/admission checks passed; the actual fixture requires CPU libopenh264 or libx264.\n";
        return 77;
    }
    std::cout << "CPU H.264 preview fixture encoder: " << h264.toStdString() << '\n';
    TestRealPreviewPlaybackSeekAndRetirement(result, h264);
    return result.failures == 0 ? 0 : 1;
}
}

REVIA_DESKTOP_SUITE("video-preview", RunVideoPreviewTests);
