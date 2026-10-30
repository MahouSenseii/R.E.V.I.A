#include "videoPreviewDialog.h"

#include "Attachments/videoDecoder.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

struct VideoPreviewDialog::Impl
{
    struct Frame
    {
        std::int64_t timestampMs = 0;
        std::string rgb;
    };

    using Source = std::function<std::optional<revia::attachments::VideoPlaybackSource>(std::stop_token)>;

    VideoPreviewDialog& owner;
    Source source;
    std::function<bool()> admitted;
    QLabel* display = nullptr;
    QLabel* status = nullptr;
    QLabel* time = nullptr;
    QPushButton* play = nullptr;
    QPushButton* pause = nullptr;
    QPushButton* stop = nullptr;
    QSlider* progress = nullptr;
    QDoubleSpinBox* seekSeconds = nullptr;
    QTimer* timer = nullptr;
    QElapsedTimer clock;
    std::jthread worker;
    mutable std::mutex mutex;
    std::condition_variable_any space;
    std::deque<Frame> frames;
    std::optional<revia::attachments::VideoInfo> metadata;
    std::string error;
    std::atomic<bool> running{false};
    std::int64_t currentMs = -1;
    std::int64_t startMs = 0;
    std::int64_t durationMs = 0;
    bool finished = false;
    bool metadataApplied = false;
    bool initialPoster = true;
    bool awaitingFrame = false;
    bool playing = false;
    bool atEnd = false;
    bool closed = false;

    Impl(VideoPreviewDialog& dialog, Source provider, std::function<bool()> admission)
        : owner(dialog), source(std::move(provider)), admitted(std::move(admission))
    {
        owner.setObjectName("videoPreviewDialog");
        auto* layout = new QVBoxLayout(&owner);
        display = new QLabel("Loading a silent preview...", &owner);
        display->setObjectName("videoPreviewFrame");
        display->setAlignment(Qt::AlignCenter);
        display->setFixedSize(revia::attachments::VideoPreviewWidth, revia::attachments::VideoPreviewHeight);
        layout->addWidget(display);
        progress = new QSlider(Qt::Horizontal, &owner);
        progress->setObjectName("videoSeekSlider");
        progress->setRange(0, 0);
        layout->addWidget(progress);
        auto* controls = new QHBoxLayout;
        play = new QPushButton("Play", &owner);
        play->setObjectName("videoPlayButton");
        pause = new QPushButton("Pause", &owner);
        pause->setObjectName("videoPauseButton");
        stop = new QPushButton("Stop", &owner);
        stop->setObjectName("videoStopButton");
        controls->addWidget(play);
        controls->addWidget(pause);
        controls->addWidget(stop);
        seekSeconds = new QDoubleSpinBox(&owner);
        seekSeconds->setObjectName("videoSeekSeconds");
        seekSeconds->setDecimals(2);
        seekSeconds->setSuffix(" s");
        seekSeconds->setRange(0, 0);
        seekSeconds->setSingleStep(0.5);
        controls->addWidget(seekSeconds);
        time = new QLabel(&owner);
        time->setObjectName("videoPreviewTime");
        controls->addWidget(time);
        controls->addStretch();
        layout->addLayout(controls);
        status = new QLabel("Silent preview; audio is not played.", &owner);
        status->setObjectName("videoPreviewStatus");
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &owner);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &owner, [&dialog] { dialog.close(); });
        QObject::connect(play, &QPushButton::clicked, &owner, [this] { Play(); });
        QObject::connect(pause, &QPushButton::clicked, &owner, [this] { Pause(); });
        QObject::connect(stop, &QPushButton::clicked, &owner, [this] { Stop(); });
        QObject::connect(progress, &QSlider::valueChanged, &owner,
            [this](const int value)
            {
                if (!progress->isSliderDown())
                    Seek(value);
            });
        QObject::connect(progress, &QSlider::sliderReleased, &owner, [this] { Seek(progress->value()); });
        QObject::connect(seekSeconds, &QDoubleSpinBox::editingFinished, &owner,
            [this] { Seek(static_cast<std::int64_t>(seekSeconds->value() * 1000.0)); });
        timer = new QTimer(&owner);
        timer->setTimerType(Qt::PreciseTimer);
        timer->setInterval((1000 + revia::attachments::VideoPreviewFramesPerSecond - 1) / revia::attachments::VideoPreviewFramesPerSecond);
        QObject::connect(timer, &QTimer::timeout, &owner, [this] { Tick(); });
        timer->start();
        UpdateControls();
    }

    ~Impl()
    {
        Close();
    }

    bool Allowed() const
    {
        try
        {
            return admitted && admitted();
        }
        catch (...)
        {
            return false;
        }
    }

    void StopWorker()
    {
        if (worker.joinable())
        {
            worker.request_stop();
            space.notify_all();
            worker.join();
        }
        running = false;
        std::lock_guard lock(mutex);
        frames.clear();
        metadata.reset();
        error.clear();
        finished = false;
    }

    void Decode(const std::int64_t requestedMs, const bool posterOnly, const std::stop_token token)
    {
        struct Finish
        {
            Impl& self;
            ~Finish()
            {
                self.running = false;
                std::lock_guard lock(self.mutex);
                self.finished = true;
            }
        } finish{*this};
        const auto fail = [&](std::string message)
        {
            std::lock_guard lock(mutex);
            error = message.substr(0, 512);
        };
        try
        {
            const auto validated = source ? source(token) : std::nullopt;
            if (token.stop_requested())
                return;
            if (!validated)
            {
                fail("The stored video is unavailable or no longer matches this conversation.");
                return;
            }
            {
                std::lock_guard lock(mutex);
                metadata = validated->info;
            }
            const revia::attachments::VideoDecoder decoder;
            const auto result = decoder.StreamPreview(
                validated->path, validated->info, requestedMs,
                [&](const std::int64_t timestampMs, const std::string& rgb)
                {
                    constexpr std::size_t bytes = revia::attachments::VideoPreviewWidth * revia::attachments::VideoPreviewHeight * 3U;
                    if (rgb.size() != bytes)
                    {
                        fail("The decoder returned an incomplete preview frame.");
                        return false;
                    }
                    std::unique_lock lock(mutex);
                    if (!space.wait(lock, token, [this] { return frames.size() < 3; }) || token.stop_requested())
                        return false;
                    frames.push_back({timestampMs, rgb});
                    return !posterOnly;
                },
                token);
            if (!token.stop_requested() && !result.ok && !result.cancelled)
                fail(result.error.empty() ? "The local video preview could not be decoded." : result.error);
        }
        catch (...)
        {
            if (!token.stop_requested())
                fail("The local video preview could not be opened.");
        }
    }

    void Start(const std::int64_t requestedMs, const bool playNow)
    {
        if (closed)
            return;
        if (!Allowed())
        {
            owner.close();
            return;
        }
        StopWorker();
        initialPoster = false;
        awaitingFrame = true;
        atEnd = false;
        metadataApplied = false;
        playing = playNow;
        startMs = std::clamp(requestedMs, std::int64_t{0}, std::max(std::int64_t{0}, durationMs - 1));
        clock.invalidate();
        status->setText("Opening the validated local video. Preview is silent.");
        running = true;
        worker =
            std::jthread([this, seek = startMs, posterOnly = !playNow](const std::stop_token token) { Decode(seek, posterOnly, token); });
        UpdateControls();
    }

    void Play()
    {
        Start(atEnd ? 0 : std::max(std::int64_t{0}, currentMs), true);
    }

    void Pause()
    {
        if (closed)
            return;
        StopWorker();
        initialPoster = false;
        awaitingFrame = false;
        playing = false;
        clock.invalidate();
        status->setText("Paused. Play resumes from the displayed frame.");
        UpdateControls();
    }

    void Stop()
    {
        Pause();
        if (!closed)
            status->setText("Preview stopped. The displayed frame is retained until you close this window.");
    }

    void Seek(const std::int64_t timestampMs)
    {
        if (!closed)
            Start(timestampMs, playing);
    }

    void UpdateControls()
    {
        play->setEnabled(!closed && !playing);
        pause->setEnabled(!closed && (playing || running));
        stop->setEnabled(!closed);
        const auto elapsed = revia::attachments::FormatVideoTimestamp(std::max(std::int64_t{0}, currentMs));
        time->setText(QString::fromStdString(elapsed + " / " + revia::attachments::FormatVideoTimestamp(durationMs)));
        if (!progress->isSliderDown())
        {
            const QSignalBlocker block(progress);
            progress->setRange(0, static_cast<int>(durationMs));
            progress->setValue(static_cast<int>(std::max(std::int64_t{0}, currentMs)));
        }
        if (!seekSeconds->hasFocus())
        {
            const QSignalBlocker block(seekSeconds);
            seekSeconds->setRange(0, static_cast<double>(durationMs) / 1000.0);
            seekSeconds->setValue(static_cast<double>(std::max(std::int64_t{0}, currentMs)) / 1000.0);
        }
    }

    void Tick()
    {
        if (closed)
            return;
        if (!Allowed())
        {
            owner.close();
            return;
        }
        if (initialPoster)
            Start(0, false);
        std::optional<Frame> next;
        std::optional<revia::attachments::VideoInfo> info;
        std::string failed;
        bool exhausted = false;
        {
            std::lock_guard lock(mutex);
            if (!metadataApplied && metadata)
                info = metadata;
            const std::int64_t due = clock.isValid() ? startMs + clock.elapsed() : currentMs;
            if (!frames.empty() && (awaitingFrame || (playing && frames.front().timestampMs <= due)))
            {
                next = std::move(frames.front());
                frames.pop_front();
            }
            failed = error;
            exhausted = finished && frames.empty();
        }
        if (info)
        {
            durationMs = std::clamp(info->durationMs, std::int64_t{0}, std::int64_t{60000});
            metadataApplied = true;
        }
        if (!failed.empty())
        {
            Pause();
            display->clear();
            currentMs = -1;
            status->setText(QString::fromStdString(failed));
        }
        else if (next)
        {
            if (!Allowed())
            {
                owner.close();
                return;
            }
            const QImage borrowed(reinterpret_cast<const uchar*>(next->rgb.data()), revia::attachments::VideoPreviewWidth,
                revia::attachments::VideoPreviewHeight, revia::attachments::VideoPreviewWidth * 3, QImage::Format_RGB888);
            display->setPixmap(QPixmap::fromImage(borrowed.copy()));
            currentMs = next->timestampMs;
            if (awaitingFrame && playing)
            {
                startMs = currentMs;
                clock.restart();
            }
            awaitingFrame = false;
            status->setText(playing ? "Playing a bounded silent preview." : "Paused. Play starts from the displayed frame.");
            space.notify_all();
        }
        if (playing && exhausted)
        {
            playing = false;
            atEnd = true;
            clock.invalidate();
            status->setText("End of silent preview.");
        }
        UpdateControls();
    }

    void Close()
    {
        if (closed)
            return;
        closed = true;
        timer->stop();
        for (QObject* child : owner.findChildren<QObject*>())
            QObject::disconnect(child, nullptr, &owner, nullptr);
        StopWorker();
        playing = false;
        clock.invalidate();
        display->clear();
        currentMs = -1;
    }
};

VideoPreviewDialog::VideoPreviewDialog(const QString& title,
    std::function<std::optional<revia::attachments::VideoPlaybackSource>(std::stop_token)> source, std::function<bool()> admitted,
    QWidget* parent)
    : QDialog(parent), impl(std::make_unique<Impl>(*this, std::move(source), std::move(admitted)))
{
    setWindowTitle(title);
}

VideoPreviewDialog::~VideoPreviewDialog() = default;

void VideoPreviewDialog::done(const int result)
{
    impl->Close();
    QDialog::done(result);
}

void VideoPreviewDialog::closeEvent(QCloseEvent* event)
{
    impl->Close();
    QDialog::closeEvent(event);
}

std::int64_t VideoPreviewDialog::CurrentTimestampMs() const
{
    return impl->currentMs;
}

bool VideoPreviewDialog::IsRunning() const
{
    return impl->running.load();
}

bool VideoPreviewDialog::IsPlaying() const
{
    return impl->playing;
}

std::size_t VideoPreviewDialog::QueuedFrameCount() const
{
    std::lock_guard lock(impl->mutex);
    return impl->frames.size();
}
