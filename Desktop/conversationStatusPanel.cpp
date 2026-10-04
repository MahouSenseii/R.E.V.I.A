#include "conversationStatusPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace
{
QLabel* StatusLabel(const char* name, QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString BoundedText(const std::string& text, const int limit)
{
    const auto value = QString::fromStdString(text);
    return value.size() > limit ? value.left(limit) + QStringLiteral("…") : value;
}
}

ConversationStatusPanel::ConversationStatusPanel(QWidget* parent) : QFrame(parent)
{
    setObjectName("conversationStatusCard");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto* layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignTop);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);
    auto* row = new QHBoxLayout();
    current = StatusLabel("conversationCurrentStatus", this);
    row->addWidget(current, 1);
    expand = new QPushButton("Details", this);
    expand->setObjectName("conversationDetailsButton");
    expand->setCheckable(true);
    row->addWidget(expand);
    review = new QPushButton("Review answer", this);
    review->setObjectName("reviewAnswerButton");
    review->setToolTip("Record your judgment of the latest displayed reply against an explicit criterion.");
    row->addWidget(review);
    layout->addLayout(row);
    outcome = StatusLabel("conversationOwnerOutcome", this);
    layout->addWidget(outcome);
    details = new QWidget(this);
    details->setObjectName("conversationStatusDetails");
    auto* detailLayout = new QVBoxLayout(details);
    detailLayout->setAlignment(Qt::AlignTop);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(5);
    timing = StatusLabel("conversationResponseTiming", details);
    quality = StatusLabel("conversationQualityDiagnostic", details);
    evidence = StatusLabel("conversationResultEvidence", details);
    detailLayout->addWidget(timing);
    detailLayout->addWidget(quality);
    detailLayout->addWidget(evidence);
    detailViewport = new QScrollArea(this);
    detailViewport->setObjectName("conversationDetailsScroll");
    detailViewport->setFrameShape(QFrame::NoFrame);
    detailViewport->setWidgetResizable(true);
    detailViewport->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailViewport->setFixedHeight(84);
    detailViewport->setWidget(details);
    layout->addWidget(detailViewport);
    connect(expand, &QPushButton::toggled, this,
        [this](const bool open)
        {
            details->setVisible(open);
            detailViewport->setVisible(open);
            expand->setText(open ? "Less" : "Details");
        });
    ResetForCompanion();
}

void ConversationStatusPanel::Observe(const revia::runtime::RuntimeEvent& event)
{
    using revia::runtime::RuntimeEventKind;
    if (event.kind == RuntimeEventKind::StateChanged)
    {
        current->setText("Conversation · " + QString::fromStdString(revia::runtime::ToString(event.state)));
        if (event.state == revia::runtime::RuntimeState::WaitingForConfirmation)
            SetOwnerOutcome("An action is waiting for your decision.");
        else if (outcome->text() == "An action is waiting for your decision.")
            SetOwnerOutcome({});
    }
    else if (event.kind == RuntimeEventKind::ComponentStatus && event.component == "Response timing")
    {
        timing->setText("Response timing · " + BoundedText(event.message, 768));
    }
    else if (event.kind == RuntimeEventKind::ComponentStatus && event.component == "Conversation quality")
    {
        quality->setText("Diagnostic quality · " + BoundedText(event.message, 512) + " Monitor signals require a separate judgment.");
        expand->setText(event.phase == "Flagged" && !expand->isChecked() ? "Details · flag" : (expand->isChecked() ? "Less" : "Details"));
    }
    else if (event.kind == RuntimeEventKind::Proposal)
    {
        SetOwnerOutcome("Owner action · " + BoundedText(event.message, 160));
        evidence->setText("Proposal evidence · " + BoundedText(event.detail, 768));
    }
    else if (event.kind == RuntimeEventKind::InvestigationChecking)
    {
        current->setText("Checking · " + BoundedText(event.message, 100));
    }
    else if (event.kind == RuntimeEventKind::InvestigationFindings)
    {
        SetOwnerOutcome("Latest finding · " + BoundedText(event.message, 160));
        evidence->setText("Finding evidence · " + BoundedText(event.detail, 768));
    }
}

void ConversationStatusPanel::ResetForCompanion()
{
    current->setText("Conversation · Offline");
    outcome->clear();
    outcome->hide();
    timing->setText("Response timing · Awaiting a measured reply.");
    quality->clear();
    evidence->clear();
    expand->setChecked(false);
    expand->setText("Details");
    details->hide();
    detailViewport->hide();
    review->setEnabled(false);
}

void ConversationStatusPanel::SetReviewAvailable(const bool available)
{
    review->setEnabled(available);
}

void ConversationStatusPanel::SetOwnerOutcome(const QString& message)
{
    outcome->setText(message.left(240));
    outcome->setVisible(!message.isEmpty());
}

void ConversationStatusPanel::ClearOwnerOutcomeIfMatching(const QString& obsoleteMessage)
{
    if (outcome->text() == obsoleteMessage)
        SetOwnerOutcome({});
}

QPushButton* ConversationStatusPanel::ReviewButton() const
{
    return review;
}
