#include "../Desktop/learningStudioPanel.h"
#include "../Desktop/developmentStudioPanel.h"
#include "../Desktop/audienceStudioPanel.h"
#include "testSupport.h"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFile>
#include <QFontDatabase>
#include <QDir>
#include <QScrollArea>
#include <QScrollBar>

#include <iostream>

namespace
{
void CapturePanel(QWidget* panel, const QString& name)
{
    const QString renderDirectory = qEnvironmentVariable("REVIA_STUDIO_PANEL_RENDER_DIR");
    QScrollArea shell;
    shell.setWidgetResizable(true);
    shell.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    shell.setFrameShape(QFrame::NoFrame);
    shell.setObjectName("rootPanel");
    shell.viewport()->setObjectName("rootPanel");
    shell.setWidget(panel);
    QDir directory = QDir::current();
    for (int depth = 0; depth < 5; ++depth)
    {
        QFile theme(directory.filePath("Desktop/revia.qss"));
        if (theme.open(QIODevice::ReadOnly))
        {
            shell.setStyleSheet(QString::fromUtf8(theme.readAll()));
            break;
        }
        directory.cdUp();
    }
    for (const int width : {480, 760, 1040})
    {
        shell.resize(width, 720);
        shell.show();
        QApplication::processEvents();
        QApplication::processEvents();
        if (panel->width() > shell.viewport()->width())
        {
            for (auto* child : panel->findChildren<QWidget*>())
                if (child->minimumSizeHint().width() > shell.viewport()->width())
                    std::cerr << name.toStdString() << " constraint: " << child->metaObject()->className() << ' '
                              << child->objectName().toStdString() << " minimum " << child->minimumSizeHint().width() << '\n';
        }
        revia::tests::Check(panel->width() <= shell.viewport()->width(),
            name.toStdString() + " content width " + std::to_string(panel->width()) + " exceeds viewport " +
                std::to_string(shell.viewport()->width()) + " at " + std::to_string(width));
        if (renderDirectory.isEmpty())
            continue;
        revia::tests::Check(QDir().mkpath(renderDirectory), "Studio render directory could not be created.");
        shell.verticalScrollBar()->setValue(0);
        QApplication::processEvents();
        revia::tests::Check(shell.grab().save(QDir(renderDirectory).filePath(name + "-" + QString::number(width) + "-top.png")),
            "Studio top capture failed.");
        shell.verticalScrollBar()->setValue(shell.verticalScrollBar()->maximum());
        QApplication::processEvents();
        revia::tests::Check(shell.grab().save(QDir(renderDirectory).filePath(name + "-" + QString::number(width) + "-details.png")),
            "Studio detail capture failed.");
    }
}
}

namespace
{
using revia::tests::Check;

void TestExactCandidateDecisionAndCompanionReset()
{
    std::string decidedId, feedback;
    auto decision = revia::learning::LearningDecision::Pending;
    LearningStudioPanel::Controls controls;
    controls.inventory = [](const std::string&) {};
    controls.update = controls.rollback = [] {};
    controls.decide = [&](const std::string& id, const auto value, const std::string& message)
    {
        decidedId = id;
        decision = value;
        feedback = message;
    };
    LearningStudioPanel panel(std::move(controls));
    revia::runtime::LearningStudioSnapshot snapshot;
    snapshot.inventorySkill = revia::skills::SkillPackageReference{"workspace-inventory", "1.0.0", std::string(64, 'a')};
    revia::learning::LearningRecord record;
    record.id = "lesson-exact-synthetic-revision";
    record.digest = std::string(64, 'b');
    record.candidate.lesson.statement = "Synthetic private candidate for review";
    record.candidate.evidence.supporting = {"Synthetic measured outcome"};
    snapshot.lessons = {record};
    panel.SetSnapshot(snapshot);
    auto* choices = panel.findChild<QComboBox*>("learningDecision");
    auto* message = panel.findChild<QPlainTextEdit*>("learningReviewFeedback");
    auto* apply = panel.findChild<QPushButton*>("learningRecordDecision");
    Check(choices && message && apply, "Learning decisions are missing from the real widget.");
    choices->setCurrentIndex(choices->findData(static_cast<int>(revia::learning::LearningDecision::Reject)));
    message->setPlainText("Measured evidence does not support acceptance.");
    apply->click();
    Check(decidedId == record.id && decision == revia::learning::LearningDecision::Reject && !feedback.empty(),
        "Widget review lost exact candidate identity or selected parent decision.");
    panel.SetBusy(true);
    Check(!apply->isEnabled(), "A busy widget permits duplicate admission requests.");
    panel.ResetForCompanion();
    Check(panel.findChild<QComboBox*>("learningCandidates")->count() == 0 && message->toPlainText().isEmpty() &&
              panel.findChild<QPlainTextEdit*>("learningCandidateDetails")->toPlainText().isEmpty(),
        "Companion switch retained private learning details.");
}

void TestDevelopmentStagesAndReset()
{
    int proposed = 0, validated = 0, reviewed = 0;
    DevelopmentStudioPanel panel({[&] { ++proposed; }, [&] { ++validated; }, [&] { ++reviewed; }});
    panel.findChild<QPushButton*>("developmentPropose")->click();
    Check(proposed == 1 && !panel.findChild<QPushButton*>("developmentReview")->isEnabled(),
        "Proposal UI implied exact validated review was available.");
    revia::improvement::DevelopmentSnapshot snapshot;
    snapshot.candidate.id = "synthetic-candidate";
    snapshot.candidate.candidateDigest = std::string(64, 'c');
    snapshot.candidate.change.path = "Desktop/studioDuration.h";
    panel.SetSnapshot(snapshot);
    panel.findChild<QPushButton*>("developmentValidate")->click();
    Check(validated == 1 && reviewed == 0, "Validation and review were collapsed into one operation.");
    snapshot.validation = revia::improvement::ValidationReceipt{};
    snapshot.validation->digest = std::string(64, 'd');
    panel.SetSnapshot(snapshot);
    panel.findChild<QPushButton*>("developmentReview")->click();
    Check(reviewed == 1, "Exact validated candidate could not request review.");
    snapshot.accepted = true;
    panel.SetSnapshot(snapshot);
    Check(panel.findChild<QLabel*>("developmentSummary")->text().contains("integration pending", Qt::CaseInsensitive),
        "Review acceptance was displayed as source integration.");
    panel.ResetForCompanion();
    Check(panel.findChild<QPlainTextEdit*>("developmentDetails")->toPlainText().isEmpty() &&
              !panel.findChild<QPushButton*>("developmentValidate")->isEnabled(),
        "Companion switch retained development candidate state.");
}

void TestRecognitionRequiresExplicitConsentAndClearsPrivateInputs()
{
    std::string matchedPerson, selectedWave;
    bool consented = false;
    AudienceStudioPanel::Controls controls;
    controls.enroll = [&](const std::string& id, const std::string& wave, const bool consent)
    {
        matchedPerson = id;
        selectedWave = wave;
        consented = consent;
    };
    AudienceStudioPanel panel(std::move(controls));
    revia::identity::RelationshipState person;
    person.entityId = "synthetic:existing-person";
    person.displayName = "Synthetic existing person";
    panel.SetSnapshot({}, {person}, {});
    auto* consent = panel.findChild<QCheckBox*>("recognitionConsent");
    auto* enroll = panel.findChild<QPushButton*>("recognitionEnroll");
    auto* wave = panel.findChild<QLineEdit*>("recognitionWave");
    Check(consent && !consent->isChecked() && !enroll->isEnabled(), "Recognition UI inferred consent from a known name.");
    wave->setText("synthetic-selected-recording.wav");
    consent->setChecked(true);
    enroll->click();
    Check(matchedPerson == person.entityId && selectedWave == "synthetic-selected-recording.wav" && consented,
        "Recognition UI lost selected existing identity or explicit consent.");
    panel.SetSnapshot({}, {}, {});
    Check(!consent->isChecked() && !enroll->isEnabled(), "Removed identity inherited an earlier consented enrollment.");
    panel.findChild<QComboBox*>("audienceKind")->setCurrentIndex(1);
    panel.ResetForCompanion();
    Check(
        !consent->isChecked() && wave->text().isEmpty() && panel.findChild<QComboBox*>("recognitionPerson")->count() == 0 &&
            panel.findChild<QComboBox*>("audienceKind")->currentData().toInt() == static_cast<int>(revia::identity::AudienceKind::Unknown),
        "Companion reset retained recognition enrollment or private audience choice.");
}

void TestControlledSnapshotsFitResponsiveLayouts()
{
    auto* learning = new LearningStudioPanel({});
    revia::runtime::LearningStudioSnapshot learned;
    learned.inventorySkill = revia::skills::SkillPackageReference{"workspace-inventory", "1.1.0", std::string(64, 'a')};
    revia::learning::LearningRecord record;
    record.id = "lesson-" + std::string(64, 'b');
    record.digest = std::string(64, 'c');
    record.candidate.lesson.statement = "Controlled lesson: type counts need the checked version and a complete listing.";
    record.candidate.lesson.evidence = "Disposable native inventory result was complete; fresh mixed-entry checks passed.";
    record.candidate.evidence.supporting = {"Controlled result bound to skill v1.1.0 and its SHA256"};
    record.checks = {record.digest, true, true, true};
    learned.lessons = {record};
    learning->SetSnapshot(learned);
    Check(learning->findChild<QPlainTextEdit*>("learningCandidateDetails")->wordWrapMode() == QTextOption::WrapAnywhere &&
              learning->findChild<QPlainTextEdit*>("learningCandidateDetails")->toPlainText().contains(QString::fromStdString(record.id)),
        "Learning detail view loses exact identity or cannot wrap its long tokens.");
    CapturePanel(learning, "learning");
    auto* development = new DevelopmentStudioPanel({});
    revia::improvement::DevelopmentSnapshot candidate;
    candidate.candidate.id = std::string(64, 'd');
    candidate.candidate.change = {"Desktop/studioDuration.h", "'f', 2)", "'f', 1)"};
    candidate.candidate.candidateDigest = std::string(64, 'e');
    candidate.candidate.sourceDigest = std::string(64, 'f');
    candidate.candidate.origin.configuredIdentity = "controlled-companion";
    candidate.candidate.origin.providerIdentity = "controlled-fixture-provider";
    candidate.validation = revia::improvement::ValidationReceipt{};
    candidate.validation->digest = std::string(64, 'a');
    candidate.validation->artifactDigest = std::string(64, 'b');
    candidate.accepted = true;
    development->SetSnapshot(candidate);
    CapturePanel(development, "development");
    auto* audience = new AudienceStudioPanel({});
    revia::identity::RelationshipState person;
    person.entityId = "synthetic:existing-person";
    person.displayName = "Synthetic existing person";
    revia::identity::RelationshipEvidenceRecord evidence;
    evidence.event.evidenceId = "controlled-evidence-01";
    evidence.event.entityId = person.entityId;
    audience->SetSnapshot({revia::identity::AudienceKind::Shared, "controlled-room", 3, {person.entityId}}, {person}, {evidence});
    CapturePanel(audience, "audience");
}
}

void RunStudioPanelTests()
{
    TestExactCandidateDecisionAndCompanionReset();
    TestDevelopmentStagesAndReset();
    TestRecognitionRequiresExplicitConsentAndClearsPrivateInputs();
    TestControlledSnapshotsFitResponsiveLayouts();
    std::cout << "Studio panel checks passed: exact parent decisions, distinct stages and companion reset.\n";
}

#ifdef REVIA_STUDIO_PANEL_STANDALONE
int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/segoeui.ttf");
    application.setFont(QFont("Segoe UI"));
    try
    {
        RunStudioPanelTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
