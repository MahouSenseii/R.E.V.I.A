#pragma once

#include "Attachments/attachmentLibrary.h"

#include <QDialog>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

class QCloseEvent;

class VideoPreviewDialog final : public QDialog
{
  public:
    VideoPreviewDialog(const QString& title, std::function<std::optional<revia::attachments::VideoPlaybackSource>(std::stop_token)> source,
        std::function<bool()> admitted, QWidget* parent = nullptr);
    ~VideoPreviewDialog() override;
    void done(int result) override;

    [[nodiscard]] std::int64_t CurrentTimestampMs() const;
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] bool IsPlaying() const;
    [[nodiscard]] std::size_t QueuedFrameCount() const;

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
