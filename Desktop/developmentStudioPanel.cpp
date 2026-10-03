#include "developmentStudioPanel.h"

#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace
{
QString Text(const std::string& value)
{
    return QString::fromStdString(value);
}
QLabel* Label(const QString& text, QWidget* parent, const char* name)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
}

DevelopmentStudioPanel::DevelopmentStudioPanel(Controls value, QWidget* parent) : QWidget(parent), controls(std::move(value))
{
    setObjectName("developmentStudioPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->addWidget(Label("Self Development", this, "sectionTitle"));
    layout->addWidget(Label("Your companion proposes one bounded presentation improvement against an approved baseline. Native checks and "
                            "a separate companion review bind the exact candidate. After the component changes, another proposal needs "
                            "a newly reviewed contract.",
        this, "secondaryText"));
    auto* stages = new QGroupBox("Presentation duration component", this);
    auto* stageLayout = new QVBoxLayout(stages);
    propose = new QPushButton("Ask companion for a proposal", stages);
    propose->setObjectName("developmentPropose");
    validate = new QPushButton("Run native candidate checks", stages);
    validate->setObjectName("developmentValidate");
    review = new QPushButton("Request exact companion review", stages);
    review->setObjectName("developmentReview");
    for (auto* button : {propose, validate, review})
        stageLayout->addWidget(button);
    layout->addWidget(stages);
    summary = Label({}, this, "developmentSummary");
    details = new QPlainTextEdit(this);
    details->setObjectName("developmentDetails");
    details->setReadOnly(true);
    details->setWordWrapMode(QTextOption::WrapAnywhere);
    details->setMinimumHeight(250);
    details->setMaximumHeight(400);
    outcome = Label({}, this, "developmentOutcome");
    layout->addWidget(summary);
    layout->addWidget(details);
    layout->addWidget(Label("Integration, packaging, activation and recovery have distinct host gates. This panel requests proposal, "
                            "validation and review only. Human-like response quality still awaits owner review.",
        this, "secondaryText"));
    layout->addWidget(outcome);
    layout->addStretch();
    connect(propose, &QPushButton::clicked, this,
        [this]
        {
            if (controls.propose)
                controls.propose();
        });
    connect(validate, &QPushButton::clicked, this,
        [this]
        {
            if (controls.validate)
                controls.validate();
        });
    connect(review, &QPushButton::clicked, this,
        [this]
        {
            if (controls.review)
                controls.review();
        });
    SetSnapshot({});
}

void DevelopmentStudioPanel::SetSnapshot(const std::optional<revia::improvement::DevelopmentSnapshot>& value)
{
    snapshot = value;
    if (!snapshot)
    {
        summary->setText("No presentation candidate. A proposal requires the current local model and the approved component baseline. "
                         "A changed baseline is refused before model generation.");
        details->clear();
    }
    else
    {
        const auto& state = *snapshot;
        summary->setText(state.incomplete   ? "A completed effect has an incomplete durable record; inspection required."
                         : state.recovered  ? "Disposable release recovered; failed candidate retained."
                         : state.activated  ? "Disposable release activated."
                         : state.packaged   ? "Packaged; activation pending."
                         : state.integrated ? "Source integrated; package pending."
                         : state.accepted   ? "Review accepted; source integration pending."
                         : state.validation ? "Exact native validation recorded; companion review pending."
                                            : "Proposal recorded; native validation pending.");
        const auto& candidate = state.candidate;
        QString text = "Candidate: " + Text(candidate.id) + "\nComponent: " + Text(candidate.change.path) +
                       "\nConfigured companion: " + Text(candidate.origin.configuredIdentity) +
                       "\nServing model: " + Text(candidate.origin.providerIdentity) +
                       "\nCandidate SHA256: " + Text(candidate.candidateDigest) + "\nSource SHA256: " + Text(candidate.sourceDigest) +
                       "\n\nFind: " + Text(candidate.change.find) + "\nReplace: " + Text(candidate.change.replace);
        if (state.validation)
            text += "\n\nValidation receipt: " + Text(state.validation->digest) +
                    "\nArtifact SHA256: " + Text(state.validation->artifactDigest);
        details->setPlainText(text);
    }
    SetBusy(busy);
}

void DevelopmentStudioPanel::SetBusy(const bool value)
{
    busy = value;
    propose->setEnabled(!busy);
    validate->setEnabled(!busy && snapshot && !snapshot->integrated && !snapshot->quarantined && !snapshot->incomplete);
    review->setEnabled(
        !busy && snapshot && snapshot->validation && !snapshot->integrated && !snapshot->quarantined && !snapshot->incomplete);
}

void DevelopmentStudioPanel::SetOutcome(const bool success, const std::string& message)
{
    outcome->setText(message.empty() ? (success ? "The request completed." : "The request was refused.") : Text(message));
}

void DevelopmentStudioPanel::ResetForCompanion()
{
    outcome->clear();
    busy = false;
    SetSnapshot({});
}
