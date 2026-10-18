#include "attachmentTray.h"

#include <QBuffer>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

AttachmentTray::AttachmentTray(std::function<void(const std::string& id)> removeHandler, QWidget* parent)
    : QWidget(parent), onRemove(std::move(removeHandler))
{
    setObjectName(QStringLiteral("attachmentTray"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    layout->addLayout(row);
    status = new QLabel(this);
    status->setObjectName(QStringLiteral("attachmentStatus"));
    status->setWordWrap(true);
    status->hide();
    layout->addWidget(status);
    Rebuild();
}

void AttachmentTray::Add(AttachmentChip chip)
{
    chips.push_back(std::move(chip));
    Rebuild();
}

void AttachmentTray::Remove(const std::string& id)
{
    chips.erase(std::remove_if(chips.begin(), chips.end(), [&](const AttachmentChip& chip) { return chip.id == id; }), chips.end());
    Rebuild();
}

void AttachmentTray::Clear()
{
    chips.clear();
    Rebuild();
}

std::vector<std::string> AttachmentTray::Ids() const
{
    std::vector<std::string> ids;
    for (const AttachmentChip& chip : chips)
        ids.push_back(chip.id);
    return ids;
}

void AttachmentTray::SetStatus(const QString& text)
{
    status->setText(text);
    status->setVisible(!text.isEmpty());
}

void AttachmentTray::Rebuild()
{
    while (QLayoutItem* item = row->takeAt(0))
    {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (const AttachmentChip& chip : chips)
    {
        auto* frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("attachmentChip"));
        frame->setProperty("attachmentId", QString::fromStdString(chip.id));
        auto* layout = new QHBoxLayout(frame);
        layout->setContentsMargins(6, 2, 2, 2);
        layout->setSpacing(4);
        if (!chip.thumbnail.isNull())
        {
            auto* preview = new QLabel(frame);
            preview->setPixmap(QPixmap::fromImage(chip.thumbnail.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            layout->addWidget(preview);
        }
        auto* label = new QLabel(chip.name + QStringLiteral("  ") + chip.size, frame);
        label->setObjectName(QStringLiteral("attachmentChipLabel"));
        label->setToolTip(chip.name);
        layout->addWidget(label);
        auto* remove = new QToolButton(frame);
        remove->setObjectName(QStringLiteral("attachmentChipRemove"));
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(QStringLiteral("Remove ") + chip.name);
        remove->setAutoRaise(true);
        const std::string id = chip.id;
        connect(remove, &QToolButton::clicked, this,
            [this, id]()
            {
                if (onRemove)
                    onRemove(id);
            });
        layout->addWidget(remove);
        row->addWidget(frame);
    }
    row->addStretch(1);
    setVisible(!chips.empty() || !status->text().isEmpty());
}

PreparedImage PrepareImageForAttachment(const QImage& source, const int maxSide)
{
    PreparedImage prepared;
    if (source.isNull())
    {
        prepared.error = QStringLiteral("The picture could not be decoded.");
        return prepared;
    }
    QImage image = source;
    if (image.width() > maxSide || image.height() > maxSide)
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // Transparency is kept; everything else becomes plain 8-bit RGB the model reads.
    image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    QBuffer buffer(&prepared.png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
    {
        prepared.png.clear();
        prepared.error = QStringLiteral("The picture could not be converted to PNG.");
        return prepared;
    }
    prepared.thumbnail = image.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return prepared;
}

std::optional<PreparedImage> PrepareImageFileForAttachment(const QString& path, const int maxSide)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    // Bounds what a hostile header can make the decoder allocate.
    reader.setAllocationLimit(256);
    if (!reader.canRead())
        return std::nullopt;
    const QImage first = reader.read();
    if (first.isNull())
        return std::nullopt;
    return PrepareImageForAttachment(first, maxSide);
}

bool LooksLikeImageFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg") ||
           suffix == QStringLiteral("gif") || suffix == QStringLiteral("webp") || suffix == QStringLiteral("bmp");
}
