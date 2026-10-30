#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QWidget>

#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class QHBoxLayout;
class QLabel;

// One file waiting to be sent with the next message.
struct AttachmentChip
{
    std::string id;
    QString name;
    QString size;
    // Images only: a small preview, shown on the chip and in the transcript.
    QImage thumbnail;
    bool video = false;
    std::int64_t durationMs = 0;
};

// The row of attachment chips above the message box. It only presents: the window
// stages and removes files through the session and tells the tray what happened.
class AttachmentTray final : public QWidget
{
  public:
    explicit AttachmentTray(std::function<void(const std::string& id)> onRemove, QWidget* parent = nullptr);

    void Add(AttachmentChip chip);
    void Remove(const std::string& id);
    void Clear();
    [[nodiscard]] std::vector<std::string> Ids() const;
    [[nodiscard]] const std::vector<AttachmentChip>& Chips() const { return chips; }
    [[nodiscard]] bool IsEmpty() const { return chips.empty(); }
    void SetStatus(const QString& text);
    // Runs after every change to the chips, so the window can enable Send for files alone.
    void SetChangeHook(std::function<void()> hook) { onChange = std::move(hook); }

  private:
    void Rebuild();

    std::function<void(const std::string& id)> onRemove;
    std::function<void()> onChange;
    std::vector<AttachmentChip> chips;
    QHBoxLayout* row = nullptr;
    QLabel* status = nullptr;
};

// A picture the user picked or pasted, made ready for the local vision model: the first
// frame, oriented, shrunk to at most `maxSide` pixels on its long side, as PNG. Decoding
// happens here with Qt's image readers -- never by handing the file to another program.
struct PreparedImage
{
    QByteArray png;
    QImage thumbnail;
    QString error;
    QString notice;
};
[[nodiscard]] PreparedImage PrepareImageForAttachment(const QImage& image, int maxSide);
[[nodiscard]] std::optional<PreparedImage> PrepareImageFileForAttachment(const QString& path, int maxSide);
// Whether the path names a picture Qt can decode here.
[[nodiscard]] bool LooksLikeImageFile(const QString& path);
