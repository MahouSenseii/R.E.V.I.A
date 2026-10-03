#include "voiceHealthPanel.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace
{
struct HealthPresentation
{
    QString badge;
    QString detail;
    QString tone;
};

HealthPresentation PresentHealth(const QString& phase)
{
    if (phase == "Degraded")
    {
        return {"Text mode",
            "Voice synthesis failed. The cause is unknown. Replies remain available as text; fresh synthesis must verify recovery.",
            "warning"};
    }
    if (phase == "Available")
    {
        return {"Synthesis ready", "Fresh voice synthesis succeeded. Speaker playback has not been verified.", "available"};
    }
    if (phase == "Disabled")
    {
        return {"Voice off", "Voice output is disabled. Replies remain available as text.", "muted"};
    }
    return {
        "Unverified", "Fresh synthesis has not verified the selected voice in this session. Replies remain available as text.", "neutral"};
}

QString PresentCue(const std::string& phase)
{
    if (phase == "Preparing")
        return "Preparing approved status clips while voice work is idle.";
    if (phase == "Prepared")
        return "A validated status clip is cached for this voice.";
    if (phase == "PreparationFailed")
        return "Status clips could not be fully prepared. Text remains available; no automatic retry is running.";
    if (phase == "Queued")
        return "A cached status clip is queued.";
    if (phase == "Playing")
        return "Playing a cached status clip.";
    if (phase == "Played")
        return "Cached playback finished; it does not verify fresh synthesis.";
    if (phase == "Cancelled")
        return "Cached status playback was cancelled.";
    if (phase == "Unavailable")
        return "Approved status clips are not fully available for this voice. Text remains available.";
    if (phase == "PlaybackFailed")
        return "Cached audio playback failed. Text remains available.";
    return {};
}
}

VoiceHealthPanel::VoiceHealthPanel(const bool compact, QWidget* parent) : QFrame(parent)
{
    compactView = compact;
    const QString prefix = compact ? "chatVoiceHealth" : "voiceHealth";
    setObjectName(prefix + "Card");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, compact ? 8 : 12, 12, compact ? 8 : 12);
    layout->setSpacing(8);
    auto* row = new QHBoxLayout();
    row->setSpacing(12);
    badge = new QLabel(this);
    badge->setObjectName(prefix + "Badge");
    badge->setAccessibleName("Current voice synthesis health");
    badge->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    row->addWidget(badge);
    detail = new QLabel(this);
    detail->setObjectName(prefix + "Detail");
    detail->setWordWrap(true);
    detail->setTextFormat(Qt::PlainText);
    detail->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (compact)
    {
        row->addWidget(detail, 1);
        detailsButton = new QPushButton("Details", this);
        detailsButton->setObjectName("voiceHealthDetailsButton");
        detailsButton->setAccessibleName("Open Voice health details");
        row->addWidget(detailsButton);
    }
    else
    {
        auto* title = new QLabel("Voice health", this);
        title->setObjectName("voiceHealthTitle");
        row->insertWidget(0, title, 1);
    }
    layout->addLayout(row);
    if (!compact)
    {
        layout->addWidget(detail);
        cues = new QLabel("Status clips have not been prepared for this selection.", this);
        cues->setObjectName("voiceCueStatus");
        cues->setWordWrap(true);
        cues->setTextFormat(Qt::PlainText);
        layout->addWidget(cues);
        updated = new QLabel("Awaiting current runtime evidence.", this);
        updated->setObjectName("voiceHealthUpdated");
        layout->addWidget(updated);
    }
    SetHealth("Unverified");
}

void VoiceHealthPanel::SetHealth(const QString& phase)
{
    const auto presentation = PresentHealth(phase);
    setProperty("healthPhase", phase);
    badge->setText(presentation.badge);
    badge->setProperty("tone", presentation.tone);
    badge->style()->unpolish(badge);
    badge->style()->polish(badge);
    QString visibleDetail = presentation.detail;
    if (compactView)
    {
        if (phase == "Degraded")
            visibleDetail = "Voice failed; cause unknown. Replies remain in text.";
        else if (phase == "Available")
            visibleDetail = "Fresh synthesis succeeded; playback is unverified.";
        else if (phase == "Disabled")
            visibleDetail = "Voice output is off. Replies remain in text.";
        else
            visibleDetail = "Voice is unverified. Replies remain in text.";
    }
    detail->setText(visibleDetail);
    badge->setToolTip(presentation.detail);
    setAccessibleDescription(presentation.detail);
}

QString VoiceHealthPanel::ApplyEvent(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind != revia::runtime::RuntimeEventKind::ComponentStatus)
        return {};
    if (event.component == "System cues")
    {
        const QString message = PresentCue(event.phase);
        if (!message.isEmpty() && cues)
            cues->setText(message);
        return message;
    }
    if (event.component != "Voice health")
        return {};
    const QString phase = QString::fromStdString(event.phase);
    if (phase != "Unverified" && phase != "Degraded" && phase != "Available" && phase != "Disabled")
        return {};
    SetHealth(phase);
    if (phase == "Unverified" && cues)
        cues->setText("Status clips have not been verified for this selection.");
    if (updated)
    {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(event.occurredAt.time_since_epoch()).count();
        updated->setText("Updated " + QDateTime::fromMSecsSinceEpoch(milliseconds).toString("HH:mm:ss"));
    }
    return detail->text();
}

QPushButton* VoiceHealthPanel::DetailsButton() const
{
    return detailsButton;
}
