#include "learningStudioPanel.h"

#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{
QString Text(const std::string& value)
{
    return QString::fromStdString(value);
}
QString Decision(const revia::learning::LearningDecision decision)
{
    using revia::learning::LearningDecision;
    switch (decision)
    {
    case LearningDecision::Accept:
        return "Accepted";
    case LearningDecision::Rework:
        return "Rework requested";
    case LearningDecision::Reject:
        return "Rejected";
    case LearningDecision::NeedEvidence:
        return "More evidence requested";
    default:
        return "Awaiting review";
    }
}
QString Disposition(const revia::learning::LearningDisposition value)
{
    using revia::learning::LearningDisposition;
    switch (value)
    {
    case LearningDisposition::TrustedMemory:
        return "Durable memory receipt recorded";
    case LearningDisposition::AcceptedAwaitingMemory:
        return "Accepted; memory receipt pending";
    case LearningDisposition::MemorySaveFailed:
        return "Accepted; memory save failed, retry available";
    default:
        return "Private candidate; outside trusted memory";
    }
}
const revia::learning::LiveCaptureSummary* FindCapture(
    const revia::runtime::LearningStudioSnapshot& snapshot, const std::string& captureId)
{
    for (const auto& capture : snapshot.captures)
        if (capture.captureId == captureId)
            return &capture;
    return nullptr;
}
QString CaptureLabel(const revia::learning::LiveCaptureSummary& capture)
{
    QString said = Text(capture.input).simplified();
    if (said.size() > 48)
        said = said.left(47) + "…";
    return (capture.decision == revia::learning::LearningDecision::Pending ? QString("Needs a correction") : Decision(capture.decision)) +
           " · " + said;
}
QLabel* Label(const QString& text, QWidget* parent, const char* name = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (name)
        label->setObjectName(name);
    return label;
}
}

LearningStudioPanel::LearningStudioPanel(Controls value, QWidget* parent) : QWidget(parent), controls(std::move(value))
{
    setObjectName("learningStudioPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->addWidget(Label("Skills & Learning", this, "sectionTitle"));
    layout->addWidget(Label("Use a checked procedure, then review what your companion learned. Private lessons and neutral exported skills "
                            "have separate decisions.",
        this, "secondaryText"));
    auto* package = new QGroupBox("Workspace inventory", this);
    auto* packageLayout = new QGridLayout(package);
    skill = Label("No checked skill selected.", package, "learningSkillPin");
    packageLayout->addWidget(skill, 0, 0, 1, 2);
    directory = new QLineEdit(package);
    directory->setObjectName("learningInventoryDirectory");
    directory->setPlaceholderText("Choose a folder permitted by your companion's capabilities");
    auto* browse = new QPushButton("Choose folder…", package);
    packageLayout->addWidget(directory, 1, 0, 1, 2);
    packageLayout->addWidget(browse, 2, 0, 1, 2);
    inventory = new QPushButton("Run pinned inventory", package);
    inventory->setObjectName("learningRunInventory");
    update = new QPushButton("Check & activate update", package);
    update->setObjectName("learningUpdate");
    rollback = new QPushButton("Roll back selection", package);
    rollback->setObjectName("learningRollback");
    exportPackage = new QPushButton("Export reviewed neutral skill", package);
    exportPackage->setObjectName("learningExport");
    packageLayout->addWidget(inventory, 3, 0, 1, 2);
    packageLayout->addWidget(update, 4, 0, 1, 2);
    packageLayout->addWidget(rollback, 5, 0, 1, 2);
    packageLayout->addWidget(exportPackage, 6, 0, 1, 2);
    layout->addWidget(package);
    auto* review = new QGroupBox("Private candidate review", this);
    auto* reviewLayout = new QVBoxLayout(review);
    candidates = new QComboBox(review);
    candidates->setObjectName("learningCandidates");
    candidates->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    candidates->setMinimumContentsLength(12);
    reviewLayout->addWidget(candidates);
    details = new QPlainTextEdit(review);
    details->setObjectName("learningCandidateDetails");
    details->setReadOnly(true);
    details->setWordWrapMode(QTextOption::WrapAnywhere);
    details->setMinimumHeight(200);
    details->setMaximumHeight(320);
    reviewLayout->addWidget(details);
    decision = new QComboBox(review);
    decision->setObjectName("learningDecision");
    for (const auto value : {revia::learning::LearningDecision::NeedEvidence, revia::learning::LearningDecision::Rework,
             revia::learning::LearningDecision::Reject, revia::learning::LearningDecision::Accept})
        decision->addItem(Decision(value), static_cast<int>(value));
    reviewLayout->addWidget(decision);
    feedback = new QPlainTextEdit(review);
    feedback->setObjectName("learningReviewFeedback");
    feedback->setPlaceholderText("Record why the measured evidence supports this decision");
    feedback->setMaximumHeight(90);
    reviewLayout->addWidget(feedback);
    recordDecision = new QPushButton("Record parent decision", review);
    recordDecision->setObjectName("learningRecordDecision");
    reviewLayout->addWidget(recordDecision);
    layout->addWidget(review);
    auto* training = new QGroupBox("Training corrections", this);
    auto* trainingLayout = new QVBoxLayout(training);
    captureStatus = Label({}, training, "learningCaptureStatus");
    trainingLayout->addWidget(captureStatus);
    captures = new QComboBox(training);
    captures->setObjectName("learningCaptures");
    captures->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    captures->setMinimumContentsLength(12);
    trainingLayout->addWidget(captures);
    captureDetails = new QPlainTextEdit(training);
    captureDetails->setObjectName("learningCaptureDetails");
    captureDetails->setReadOnly(true);
    captureDetails->setMinimumHeight(160);
    captureDetails->setMaximumHeight(280);
    trainingLayout->addWidget(captureDetails);
    captureDecision = new QComboBox(training);
    captureDecision->setObjectName("learningCaptureDecision");
    captureDecision->addItem("Accept: train on my correct reply", static_cast<int>(revia::learning::LearningDecision::Accept));
    captureDecision->addItem("Rework: keep it as a draft", static_cast<int>(revia::learning::LearningDecision::Rework));
    captureDecision->addItem("Need evidence: decide later", static_cast<int>(revia::learning::LearningDecision::NeedEvidence));
    captureDecision->addItem("Reject: never train on this", static_cast<int>(revia::learning::LearningDecision::Reject));
    trainingLayout->addWidget(captureDecision);
    correction = new QPlainTextEdit(training);
    correction->setObjectName("learningCaptureCorrection");
    correction->setPlaceholderText("The reply your companion should have given");
    correction->setMinimumHeight(90);
    correction->setMaximumHeight(180);
    trainingLayout->addWidget(correction);
    auto* captureButtons = new QHBoxLayout;
    saveCorrection = new QPushButton("Save correction", training);
    saveCorrection->setObjectName("learningSaveCorrection");
    deleteCapture = new QPushButton("Delete capture…", training);
    deleteCapture->setObjectName("learningDeleteCapture");
    captureButtons->addWidget(saveCorrection);
    captureButtons->addWidget(deleteCapture);
    trainingLayout->addLayout(captureButtons);
    layout->addWidget(training);
    outcome = Label({}, this, "learningOutcome");
    layout->addWidget(outcome);
    layout->addStretch();
    connect(browse, &QPushButton::clicked, this,
        [this]
        {
            const auto path = QFileDialog::getExistingDirectory(this, "Inventory folder");
            if (!path.isEmpty())
                directory->setText(path);
        });
    connect(inventory, &QPushButton::clicked, this,
        [this]
        {
            if (controls.inventory)
                controls.inventory(directory->text().toStdString());
        });
    connect(update, &QPushButton::clicked, this,
        [this]
        {
            if (controls.update)
                controls.update();
        });
    connect(rollback, &QPushButton::clicked, this,
        [this]
        {
            if (controls.rollback)
                controls.rollback();
        });
    connect(exportPackage, &QPushButton::clicked, this,
        [this]
        {
            if (controls.exportPackage)
                controls.exportPackage();
        });
    connect(candidates, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this]
        {
            feedback->clear();
            ShowCandidate();
        });
    connect(recordDecision, &QPushButton::clicked, this,
        [this]
        {
            if (controls.decide && !candidates->currentData().toString().isEmpty())
                controls.decide(candidates->currentData().toString().toStdString(),
                    static_cast<revia::learning::LearningDecision>(decision->currentData().toInt()), feedback->toPlainText().toStdString());
        });
    connect(captures, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { ShowCapture(true); });
    connect(saveCorrection, &QPushButton::clicked, this,
        [this]
        {
            if (controls.correct && !shownCapture.empty())
                controls.correct(shownCapture, static_cast<revia::learning::LearningDecision>(captureDecision->currentData().toInt()),
                    correction->toPlainText().toStdString());
        });
    connect(deleteCapture, &QPushButton::clicked, this,
        [this]
        {
            if (!controls.discard || shownCapture.empty())
                return;
            const std::string id = shownCapture;
            if (QMessageBox::question(this, "Delete capture",
                    "Delete this kept exchange and its correction? It is also listed as withdrawn, so a dataset already "
                    "exported with it is refused.") == QMessageBox::Yes)
                controls.discard(id);
        });
    SetSnapshot({});
}

void LearningStudioPanel::SetSnapshot(const revia::runtime::LearningStudioSnapshot& value)
{
    snapshot = value;
    skill->setText(snapshot.inventorySkill ? Text(snapshot.inventorySkill->id) + " · v" + Text(snapshot.inventorySkill->version) +
                                                 "\nPinned SHA256: " + Text(snapshot.inventorySkill->digest)
                                           : Text(snapshot.status));
    const auto selected = candidates->currentData();
    const QSignalBlocker block(candidates);
    candidates->clear();
    for (const auto& record : snapshot.lessons)
        candidates->addItem(Decision(record.decision) + " · " + Text(record.id).left(22), Text(record.id));
    const int restored = candidates->findData(selected);
    if (restored >= 0)
        candidates->setCurrentIndex(restored);
    if (selected != candidates->currentData())
        feedback->clear();
    ShowCandidate();
    captureStatus->setText(Text(snapshot.captureStatus));
    {
        const QSignalBlocker blockCaptures(captures);
        captures->clear();
        for (const auto& capture : snapshot.captures)
            captures->addItem(CaptureLabel(capture), Text(capture.captureId));
        const int kept = captures->findData(Text(shownCapture));
        if (kept >= 0)
            captures->setCurrentIndex(kept);
    }
    ShowCapture(captures->currentData().toString().toStdString() != shownCapture);
    SetBusy(busy);
}

void LearningStudioPanel::ShowCapture(const bool selectionChanged)
{
    const auto id = captures->currentData().toString().toStdString();
    const auto* capture = FindCapture(snapshot, id);
    shownCapture = capture ? id : std::string{};
    if (!capture)
    {
        captureDetails->clear();
        correction->clear();
        captureDecision->setCurrentIndex(0);
    }
    else
    {
        QString text = "You said:\n" + Text(capture->input) + "\n\nThe reply you judged:\n" + Text(capture->failedOutput);
        if (capture->repaired)
            text += "\n\nThe model's own text, before style repair changed it:\n" + Text(capture->modelOutput);
        text += "\n\nYour judgment (" + Text(capture->issue) + "): " + Text(capture->criterion) + "\nEvidence: " + Text(capture->evidence);
        text += "\n\nCapture " + Text(capture->captureId) +
                (capture->revision == 0 ? QString(" · no correction saved")
                                        : " · " + Decision(capture->decision) + ", revision " + QString::number(capture->revision));
        captureDetails->setPlainText(text);
        if (selectionChanged)
        {
            correction->setPlainText(Text(capture->correction));
            const int saved = captureDecision->findData(static_cast<int>(capture->decision));
            captureDecision->setCurrentIndex(saved >= 0 ? saved : 0);
        }
    }
    saveCorrection->setEnabled(!busy && capture != nullptr);
    deleteCapture->setEnabled(!busy && capture != nullptr);
}

void LearningStudioPanel::ShowCandidate()
{
    const auto id = candidates->currentData().toString().toStdString();
    details->clear();
    for (const auto& record : snapshot.lessons)
    {
        if (record.id != id)
            continue;
        QString text = "Candidate: " + Text(record.id) + "\nSHA256: " + Text(record.digest) + "\n" + Decision(record.decision) + " · " +
                       Disposition(record.disposition) + "\n\n" + Text(record.candidate.lesson.statement) +
                       "\n\nEvidence: " + Text(record.candidate.lesson.evidence);
        for (const auto& item : record.candidate.evidence.supporting)
            text += "\nSupports: " + Text(item);
        for (const auto& item : record.candidate.evidence.contradicting)
            text += "\nContradicts: " + Text(item);
        text += "\nChecks: verified " + QString(record.checks.verified ? "yes" : "pending") + ", privacy " +
                QString(record.checks.privacySafe ? "yes" : "pending") + ", deduplicated " +
                QString(record.checks.deduplicated ? "yes" : "pending");
        if (!record.memoryId.empty())
            text += "\nMemory receipt: " + Text(record.memoryId);
        if (!record.reviews.empty())
            text += "\nLast review: " + Text(record.reviews.back().feedback);
        details->setPlainText(text);
        recordDecision->setEnabled(!busy && record.decision != revia::learning::LearningDecision::Reject &&
                                   record.disposition != revia::learning::LearningDisposition::TrustedMemory);
        return;
    }
    recordDecision->setEnabled(false);
}

void LearningStudioPanel::SetBusy(const bool value)
{
    busy = value;
    inventory->setEnabled(!busy && snapshot.inventorySkill.has_value());
    update->setEnabled(!busy);
    rollback->setEnabled(!busy && snapshot.inventorySkill.has_value());
    exportPackage->setEnabled(!busy && snapshot.inventorySkill.has_value());
    decision->setEnabled(!busy);
    ShowCandidate();
    captureDecision->setEnabled(!busy);
    correction->setReadOnly(busy);
    saveCorrection->setEnabled(!busy && !shownCapture.empty());
    deleteCapture->setEnabled(!busy && !shownCapture.empty());
}

void LearningStudioPanel::SetOutcome(const bool success, const std::string& message)
{
    outcome->setText(message.empty() ? (success ? "The request completed." : "The request was refused.") : Text(message));
}

void LearningStudioPanel::ResetForCompanion()
{
    directory->clear();
    feedback->clear();
    outcome->clear();
    decision->setCurrentIndex(0);
    busy = false;
    // Another companion's private exchange and draft go with its selection.
    shownCapture.clear();
    correction->clear();
    SetSnapshot({});
}
