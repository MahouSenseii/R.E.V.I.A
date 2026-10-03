#include "audienceStudioPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{
QString Text(const std::string& value)
{
    return QString::fromStdString(value);
}
QLabel* Label(const QString& text, QWidget* parent, const char* name = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (name)
        label->setObjectName(name);
    return label;
}
}

AudienceStudioPanel::AudienceStudioPanel(Controls value, QWidget* parent) : QWidget(parent), controls(std::move(value))
{
    setObjectName("audienceStudioPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->addWidget(Label("Audience & Recognition", this, "sectionTitle"));
    layout->addWidget(Label("Choose who may receive this conversation. Speaker matching supports continuity; it never grants owner "
                            "authority or creates a private audience.",
        this, "secondaryText"));
    summary = Label({}, this, "audienceSummary");
    layout->addWidget(summary);
    auto* room = new QGroupBox("Disclosure context", this);
    auto* roomLayout = new QVBoxLayout(room);
    kind = new QComboBox(room);
    kind->setObjectName("audienceKind");
    for (const auto value : {revia::identity::AudienceKind::Unknown, revia::identity::AudienceKind::Private,
             revia::identity::AudienceKind::Shared, revia::identity::AudienceKind::Public})
        kind->addItem(Text(revia::identity::ToString(value)), static_cast<int>(value));
    audienceId = new QLineEdit(room);
    audienceId->setObjectName("audienceScope");
    audienceId->setPlaceholderText("Audience identifier, such as local-room");
    recipients = new QComboBox(room);
    recipients->setObjectName("audienceRecipient");
    recipients->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    recipients->setMinimumContentsLength(12);
    auto* apply = new QPushButton("Apply audience context", room);
    apply->setObjectName("audienceApply");
    roomLayout->addWidget(kind);
    roomLayout->addWidget(audienceId);
    roomLayout->addWidget(recipients);
    roomLayout->addWidget(apply);
    operations.push_back(apply);
    layout->addWidget(room);
    auto* recognition = new QGroupBox("Existing person and explicit voice consent", this);
    auto* recognitionLayout = new QVBoxLayout(recognition);
    people = new QComboBox(recognition);
    people->setObjectName("recognitionPerson");
    people->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    people->setMinimumContentsLength(12);
    wave = new QLineEdit(recognition);
    wave->setObjectName("recognitionWave");
    wave->setReadOnly(true);
    wave->setPlaceholderText("Select an existing WAV recording");
    auto* browse = new QPushButton("Choose consented recording…", recognition);
    consent = new QCheckBox("I consent to local speaker matching", recognition);
    consent->setObjectName("recognitionConsent");
    consent->setChecked(false);
    consent->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    consent->setToolTip(consent->text());
    recognitionLayout->addWidget(people);
    recognitionLayout->addWidget(wave);
    recognitionLayout->addWidget(browse);
    recognitionLayout->addWidget(
        Label("The local template is derived from this selected recording. Typing a name is not enrollment. Forget removes matching "
              "consent and the stored template; physical microphone accuracy remains unverified.",
            recognition));
    recognitionLayout->addWidget(consent);
    enroll = new QPushButton("Enroll selected person", recognition);
    enroll->setObjectName("recognitionEnroll");
    auto* forget = new QPushButton("Forget speaker template & consent", recognition);
    forget->setObjectName("recognitionForget");
    recognitionLayout->addWidget(enroll);
    recognitionLayout->addWidget(forget);
    operations.push_back(forget);
    layout->addWidget(recognition);
    auto* disclosure = new QGroupBox("Contextual alias and mistaken identity", this);
    auto* disclosureLayout = new QVBoxLayout(disclosure);
    alias = new QLineEdit(disclosure);
    alias->setObjectName("audienceAlias");
    alias->setPlaceholderText("Alias shared only with the selected audience recipient");
    auto* saveAlias = new QPushButton("Share alias in this context", disclosure);
    saveAlias->setObjectName("audienceSaveAlias");
    evidence = new QComboBox(disclosure);
    evidence->setObjectName("identityEvidence");
    evidence->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    evidence->setMinimumContentsLength(12);
    auto* correct = new QPushButton("Assign selected evidence to selected person", disclosure);
    correct->setObjectName("identityCorrect");
    disclosureLayout->addWidget(alias);
    disclosureLayout->addWidget(saveAlias);
    disclosureLayout->addWidget(evidence);
    evidenceDetails = new QPlainTextEdit(disclosure);
    evidenceDetails->setObjectName("identityEvidenceDetails");
    evidenceDetails->setReadOnly(true);
    evidenceDetails->setWordWrapMode(QTextOption::WrapAnywhere);
    evidenceDetails->setMinimumHeight(140);
    evidenceDetails->setMaximumHeight(220);
    disclosureLayout->addWidget(evidenceDetails);
    disclosureLayout->addWidget(correct);
    operations.push_back(saveAlias);
    operations.push_back(correct);
    layout->addWidget(disclosure);
    outcome = Label({}, this, "audienceOutcome");
    layout->addWidget(outcome);
    layout->addStretch();
    connect(apply, &QPushButton::clicked, this,
        [this]
        {
            revia::identity::AudienceContext selected;
            selected.kind = static_cast<revia::identity::AudienceKind>(kind->currentData().toInt());
            selected.audienceId = audienceId->text().trimmed().toStdString();
            if (!recipients->currentData().toString().isEmpty())
                selected.recipientEntityIds = {recipients->currentData().toString().toStdString()};
            if (controls.audience)
                controls.audience(std::move(selected));
        });
    connect(browse, &QPushButton::clicked, this,
        [this]
        {
            const auto path = QFileDialog::getOpenFileName(this, "Consented speaker recording", {}, "WAV recordings (*.wav)");
            if (!path.isEmpty())
            {
                wave->setText(path);
                consent->setChecked(false);
            }
            SetBusy(busy);
        });
    connect(consent, &QCheckBox::toggled, this, [this] { SetBusy(busy); });
    connect(people, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this]
        {
            consent->setChecked(false);
            SetBusy(busy);
        });
    connect(enroll, &QPushButton::clicked, this,
        [this]
        {
            if (consent->isChecked() && controls.enroll)
                controls.enroll(people->currentData().toString().toStdString(), wave->text().toStdString(), true);
        });
    connect(forget, &QPushButton::clicked, this,
        [this]
        {
            if (controls.forget)
                controls.forget(people->currentData().toString().toStdString());
        });
    connect(saveAlias, &QPushButton::clicked, this,
        [this]
        {
            if (controls.alias)
                controls.alias(people->currentData().toString().toStdString(), alias->text().toStdString());
        });
    connect(correct, &QPushButton::clicked, this,
        [this]
        {
            if (controls.correct)
                controls.correct(evidence->currentData().toString().toStdString(), people->currentData().toString().toStdString());
        });
    connect(evidence, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { ShowEvidence(); });
    SetSnapshot({}, {}, {});
}

void AudienceStudioPanel::SetSnapshot(const revia::identity::AudienceContext& audience,
    const std::vector<revia::identity::RelationshipState>& known, const std::vector<revia::identity::RelationshipEvidenceRecord>& retained)
{
    summary->setText("Current audience: " + Text(revia::identity::ToString(audience.kind)) + " · " + Text(audience.audienceId) +
                     "\nRevision " + QString::number(audience.revision) + ". Human-like response quality awaits owner review.");
    if (audience.revision != observedRevision)
    {
        observedRevision = audience.revision;
        kind->setCurrentIndex(kind->findData(static_cast<int>(audience.kind)));
        audienceId->setText(Text(audience.audienceId));
    }
    const auto selected = people->currentData(), recipient = recipients->currentData(), selectedEvidence = evidence->currentData();
    const QSignalBlocker peopleBlock(people), recipientBlock(recipients), evidenceBlock(evidence);
    people->clear();
    recipients->clear();
    recipients->addItem("No identified recipient", "");
    for (const auto& person : known)
    {
        const auto name = person.displayName.empty() ? Text(person.entityId) : Text(person.displayName);
        people->addItem(name, Text(person.entityId));
        recipients->addItem(name, Text(person.entityId));
    }
    int index = people->findData(selected);
    if (index >= 0)
        people->setCurrentIndex(index);
    if (selected != people->currentData())
        consent->setChecked(false);
    index = recipients->findData(recipient);
    if (index >= 0)
        recipients->setCurrentIndex(index);
    evidence->clear();
    retainedEvidence = retained;
    for (const auto& record : retained)
        evidence->addItem(
            Text(record.event.evidenceId) + (record.corrected ? " · corrected" : " · observed"), Text(record.event.evidenceId));
    index = evidence->findData(selectedEvidence);
    if (index >= 0)
        evidence->setCurrentIndex(index);
    ShowEvidence();
    SetBusy(busy);
}

void AudienceStudioPanel::ShowEvidence()
{
    evidenceDetails->clear();
    const auto id = evidence->currentData().toString().toStdString();
    for (const auto& record : retainedEvidence)
    {
        if (record.event.evidenceId != id)
            continue;
        evidenceDetails->setPlainText("Evidence: " + Text(id) + "\nAttributed person: " + Text(record.event.entityId) +
                                      "\nOriginal attribution: " + Text(record.originalEntityId) + "\n" + Text(record.event.description) +
                                      (record.corrected ? "\nCorrection recorded; unrelated history retained."
                                                        : "\nObserved continuity evidence; no authority granted."));
        return;
    }
}

void AudienceStudioPanel::SetBusy(const bool value)
{
    busy = value;
    for (auto* operation : operations)
        operation->setEnabled(!busy);
    enroll->setEnabled(!busy && consent->isChecked() && !wave->text().isEmpty() && !people->currentData().toString().isEmpty());
}

void AudienceStudioPanel::SetOutcome(const bool success, const std::string& message)
{
    outcome->setText(message.empty() ? (success ? "The request completed." : "The request was refused.") : Text(message));
}

void AudienceStudioPanel::ResetForCompanion()
{
    consent->setChecked(false);
    wave->clear();
    alias->clear();
    audienceId->clear();
    outcome->clear();
    observedRevision = 0;
    kind->setCurrentIndex(0);
    busy = false;
    SetSnapshot({}, {}, {});
}
