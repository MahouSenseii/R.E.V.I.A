#include "reviaWindow.h"
#include "desktopSuites.h"
#include "panelWindow.h"
#include "settingsDialog.h"
#include "commandPalette.h"
#include "shellSidebar.h"
#include "serialWorker.h"
#include "tabNavigation.h"
#include "desktopObjectNames.h"
#include "theme.h"
#include "agentStudioPanel.h"
#include "audienceStudioPanel.h"
#include "developmentStudioPanel.h"
#include "learningStudioPanel.h"
#include "answerFeedbackDialog.h"
#include "attachmentTray.h"
#include "canvasPanel.h"
#include "chatMarkdown.h"
#include "chatSessionsPanel.h"
#include "documentsDialog.h"
#include "videoPreviewDialog.h"
#include "Memory/conversationArchive.h"
#include "memoryPanel.h"
#include "Memory/longTermMemory.h"
#include "Audit/contentDigest.h"
#include "Policy/desktopAuthorization.h"
#include "reviaSessionTestAccess.h"
#include "Core/runtimePath.h"
#include "Core/configManager.h"
#include "Core/appSettings.h"
#include "Core/profile.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QColor>
#include <QEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QImage>
#include <QImageReader>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegion>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTextFrame>
#include <QTextDocument>
#include <QWidgetAction>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <stdexcept>

struct ReviaWindowStopTests
{
    static void CaptureStudioDetails(ReviaWindow& window, const QString& phase, int& failures)
    {
        const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
        if (renderDirectory.isEmpty())
            return;
        auto* scroll = window.findChild<QScrollArea*>("agentStudioScroll");
        auto* details = window.findChild<QLabel*>("agentDetails");
        if (!scroll || !details)
        {
            std::cerr << "FAIL: Studio detail capture has no current viewport\n";
            ++failures;
            return;
        }
        revia::desktop::SelectNavigationPage(scroll);
        for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
        {
            scroll->window()->resize(size);
            window.agentStudioPanel->SetSnapshot(window.Session().AgentWorkflowSnapshot());
            SettleLayouts();
            const int bottom = details->mapTo(scroll->widget(), QPoint(0, 0)).y() + details->height();
            scroll->verticalScrollBar()->setValue(std::max(0, bottom - scroll->viewport()->height()));
            SettleLayouts();
            const auto name = QString("agent-studio-%1-details-%2x%3.png").arg(phase).arg(size.width()).arg(size.height());
            if (!scroll->window()->grab().save(QDir(renderDirectory).filePath(name)))
            {
                std::cerr << "FAIL: Studio detail capture failed\n";
                ++failures;
            }
        }
    }

    static void RunCompanionStudio(ReviaWindow& window, int& failures)
    {
        using namespace revia::runtime;
        using namespace revia::agents;
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        const auto wait = [&](const std::function<bool()>& ready)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!ready() && std::chrono::steady_clock::now() < until)
            {
                QCoreApplication::processEvents();
                if (!window.switchingCompanion)
                    window.Session().PollBackgroundEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            expect(ready(), "the bounded native UI operation did not finish");
        };
        const auto original = window.Session().Stamp();
        window.findChild<QPlainTextEdit*>("agentObjective")->setPlainText("A private objective sentinel");
        window.findChild<QPlainTextEdit*>("agentRevisedInput")->setPlainText("A private recovery sentinel");
        window.findChild<QPlainTextEdit*>("agentEvidence")->setPlainText("A private evidence sentinel");
        window.findChild<QLabel*>("agentFeedback")->setText("A private feedback sentinel");
        const auto ledger = window.Session().Authority();
        revia::policy::AuthorityPermissions denialPermissions;
        denialPermissions.operations = {revia::actions::ActionType::CreateDirectory};
        denialPermissions.roots = {window.Session().Paths().InstallRoot()};
        const auto denial = ledger->Deny({{"legacy", {}, 0}, denialPermissions});
        expect(!denial.empty(), "companion denial could not be installed");
        const auto exerciseAction = [&](ReviaSession& owner, const std::string& name, const bool allowed)
        {
            const auto configuration = owner.Paths().InstallRoot() / "capabilities-fixture.json";
            std::ofstream(configuration) << nlohmann::json{{"mode", "supervised"},
                {"approvedRoots", {revia::actions::PathToUtf8(owner.Paths().InstallRoot())}},
                {"createMissingApprovedRoots",
                    false}}.dump();
            std::string actionError;
            auto& runtime = ReviaSessionTestAccess::Actions(owner);
            expect(runtime.Initialize(configuration, owner.Paths().Resolve("Audit/native-fixture.jsonl"), actionError),
                "native authority runtime initialization failed");
            revia::actions::ActionRequest request;
            request.id = revia::actions::NewActionId();
            request.type = revia::actions::ActionType::CreateDirectory;
            request.source = owner.Paths().InstallRoot() / name;
            expect(!owner.IsStarted(), "companion switch fixture begins with its runtime stopped");
            const auto stopped = runtime.Execute(request, true);
            expect(!stopped.result.attempted && !stopped.Succeeded() && !std::filesystem::exists(request.source),
                "stopped companion cannot authorize a native effect");
            const auto outcome = [&]()
            {
                // Admit only this fixture action without starting model or device services.
                struct RestoreAdmission
                {
                    ReviaSession& owner;
                    bool wasStarted;
                    ~RestoreAdmission() { ReviaSessionTestAccess::MarkStudioStarted(owner, wasStarted); }
                } restore{owner, owner.IsStarted()};
                ReviaSessionTestAccess::MarkStudioStarted(owner, true);
                (void)ReviaSessionTestAccess::StudioInput(owner, revia::agents::InputSource::Typed);
                request.id = revia::actions::NewActionId();
                return runtime.Execute(request, true);
            }();
            const bool matched = outcome.Succeeded() == allowed && std::filesystem::exists(request.source) == allowed;
            if (!matched)
                std::cerr << "Companion action " << name << ": attempted=" << outcome.result.attempted
                          << ", succeeded=" << outcome.Succeeded() << ", reason=" << outcome.Message() << '\n';
            expect(matched,
                "companion selection lost its denial or narrowed the other companion");
            if (!allowed)
                expect(outcome.Message().find("explicit companion denial") != std::string::npos,
                    "original companion is refused by its retained denial, not an unrelated admission failure");
        };
        const auto cwd = QDir::currentPath();
        memoryDecision finding;
        finding.bSuccess = true;
        finding.bShouldRemember = true;
        finding.summary = "Original companion private orbit sentinel";
        finding.category = "fact";
        longTermMemory memory(window.Session().Paths().Resolve("Memory/revia_memory.db").string());
        bool added = false;
        expect(memory.Save(finding, added), "native companion sentinel could not be saved");
        revia::runtime::CompanionDescriptor second;
        std::string error;
        expect(window.companions->Create("Second companion", "assistant", second, error), "native fixture companion creation failed");

        LearningStudioSnapshot learning;
        revia::learning::LearningRecord lesson;
        lesson.id = "private-learning-selection-A";
        lesson.candidate.lesson.statement = "Private learning detail sentinel A";
        learning.lessons = {lesson};
        revia::learning::LiveCaptureSummary kept;
        kept.captureId = "0123456789abcdef";
        kept.input = "Private capture input sentinel A";
        kept.failedOutput = "Private capture reply sentinel A";
        kept.criterion = "Private capture criterion sentinel A";
        learning.captures = {kept};
        learning.captureStatus = "1 kept, 0 decided.";
        window.learningStudioPanel->SetSnapshot(learning);
        window.findChild<QPlainTextEdit*>("learningCaptureCorrection")->setPlainText("Private correction draft sentinel A");
        // The panel is refreshed every second; a refresh of the same capture keeps the draft.
        window.learningStudioPanel->SetSnapshot(learning);
        expect(window.findChild<QPlainTextEdit*>("learningCaptureCorrection")->toPlainText() == "Private correction draft sentinel A" &&
                   window.findChild<QPlainTextEdit*>("learningCaptureDetails")->toPlainText().contains("Private capture reply sentinel A") &&
                   window.findChild<QPushButton*>("learningSaveCorrection")->isEnabled(),
            "a refresh discarded the owner's correction draft, or the kept exchange was not shown for correction");
        window.findChild<QLineEdit*>("learningInventoryDirectory")->setText("private-inventory-draft-A");
        window.findChild<QPlainTextEdit*>("learningReviewFeedback")->setPlainText("Private learning feedback sentinel A");
        window.learningStudioPanel->SetOutcome(false, "Private learning outcome sentinel A");

        revia::improvement::DevelopmentSnapshot development;
        development.candidate.id = "private-development-selection-A";
        window.developmentStudioPanel->SetSnapshot(development);
        window.developmentStudioPanel->SetOutcome(false, "Private development outcome sentinel A");

        revia::identity::RelationshipState person;
        person.entityId = "private-person-selection-A";
        person.displayName = "Synthetic selected person A";
        revia::identity::RelationshipEvidenceRecord evidence;
        evidence.event.entityId = person.entityId;
        evidence.event.evidenceId = "private-evidence-selection-A";
        evidence.event.description = "Private identity evidence detail sentinel A";
        window.audienceStudioPanel->SetSnapshot({revia::identity::AudienceKind::Private, "private-audience-A", 51}, {person}, {evidence});
        window.findChild<QComboBox*>("audienceRecipient")->setCurrentIndex(1);
        window.findChild<QLineEdit*>("recognitionWave")->setText("private-recording-selection-A.wav");
        window.findChild<QLineEdit*>("audienceAlias")->setText("private-alias-draft-A");
        window.findChild<QCheckBox*>("recognitionConsent")->setChecked(true);
        window.audienceStudioPanel->SetOutcome(false, "Private audience outcome sentinel A");
        expect(window.findChild<QComboBox*>("learningCandidates")->currentData().toString() == QString::fromStdString(lesson.id) &&
                   window.findChild<QPlainTextEdit*>("developmentDetails")->toPlainText().contains("private-development-selection-A") &&
                   window.findChild<QComboBox*>("identityEvidence")->currentData().toString() ==
                       QString::fromStdString(evidence.event.evidenceId) &&
                   window.findChild<QCheckBox*>("recognitionConsent")->isChecked(),
            "new Studio private-selection fixtures were not populated before the actual companion switch");
        RuntimeEvent lateReply{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "late-original-ui-sentinel"};
        lateReply.audienceRevision = window.Session().Audience().revision;
        window.Session().Events().Publish(lateReply);
        if (auto* review = window.findChild<QPushButton*>("reviewAnswerButton"); review && review->isEnabled())
        {
            review->click();
            if (auto* criterion = window.findChild<QLineEdit*>("answerFeedbackCriterion"))
                criterion->setText("Private outgoing answer criterion A");
        }
        std::atomic<bool> releaseSwitch = false;
        window.operationWorker = std::jthread(
            [&]()
            {
                while (!releaseSwitch.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            });
        // A Settings write still queued when the switch begins answers under the outgoing companion
        // and is dropped; the control it disabled must not stay disabled afterwards.
        window.settingsWorker->Post(
            [&releaseSwitch]()
            {
                while (!releaseSwitch.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            });
        window.cloudUseForCombo->setEnabled(true);
        emit window.cloudUseForCombo->activated(1);
        expect(!window.cloudUseForCombo->isEnabled(), "the cloud choice must wait while its write is queued");
        emit window.imageMinTokensCombo->activated(window.imageMinTokensCombo->findData(QStringLiteral("1024")));
        expect(!window.imageMinTokensCombo->isEnabled(), "the picture-detail choice must wait while its write is queued");
        window.SwitchCompanion(second.id);
        expect(window.navigationLocked && !window.navigationSidebar->isEnabled() && !window.chatColumn->isEnabled(),
            "mutable companion controls remain enabled while the outgoing runtime retires");
        const auto heldWorker = window.operationWorker.get_id();
        window.messageInput->setPlainText("outgoing draft sentinel");
        window.SendMessage();
        window.StartRuntime();
        window.RunMicrophoneTest();
        window.CreateVoicePreset();
        expect(window.operationWorker.get_id() == heldWorker && !window.voiceOperationRunning.load() &&
                   window.messageInput->toPlainText() == "outgoing draft sentinel",
            "a handler started new work during companion retirement");
        releaseSwitch.store(true);
        wait([&]() { return !window.switchingCompanion; });
        expect(window.Session().Stamp().companionId == second.id && window.chatEntries.empty(), "queued A result reached selected B UI");
        expect(!window.cloudUseForCombo->testAttribute(Qt::WA_ForceDisabled),
            "a Settings write queued before a companion switch must not leave its control disabled");
        expect(!window.imageMinTokensCombo->testAttribute(Qt::WA_ForceDisabled),
            "a picture-detail write queued before retirement left the incoming companion's choice disabled");
        window.findChild<QPushButton*>("voiceHealthDetailsButton")->click();
        expect(window.findChild<QWidget*>("voiceTab")->isVisible(), "Voice Details must still reach Voice after companion replacement");
        for (auto* group : window.findChildren<QTabWidget*>())
            expect(!group->tabBar()->usesScrollButtons(), "rebuilt companion views must preserve arrow-free navigation");
        if (auto* review = window.findChild<QPushButton*>("reviewAnswerButton"))
        {
            expect(!review->isEnabled() && !window.findChild<QPlainTextEdit*>("answerFeedbackTarget"),
                "B conversation retained A's displayed answer or private review draft");
            expect(window.findChild<QLabel*>("conversationQualityDiagnostic")->text().isEmpty(),
                "B conversation retained A's quality diagnostic");
        }
        expect(window.Session().SearchMemories("orbit", 20).empty(), "B UI recall borrowed A memory");
        expect(window.findChild<QPlainTextEdit*>("agentObjective")->toPlainText().isEmpty() &&
                   window.findChild<QPlainTextEdit*>("agentRevisedInput")->toPlainText().isEmpty() &&
                   window.findChild<QPlainTextEdit*>("agentEvidence")->toPlainText().isEmpty() &&
                   window.findChild<QLabel*>("agentFeedback")->text().isEmpty(),
             "B Studio retained A's private drafts or feedback");
        expect(window.findChild<QLineEdit*>("learningInventoryDirectory")->text().isEmpty() &&
                   window.findChild<QPlainTextEdit*>("learningReviewFeedback")->toPlainText().isEmpty() &&
                   window.findChild<QPlainTextEdit*>("learningCandidateDetails")->toPlainText().isEmpty() &&
                   window.findChild<QComboBox*>("learningCandidates")->count() == 0 &&
                   window.findChild<QLabel*>("learningOutcome")->text().isEmpty(),
            "B Skills and Learning retained A's private path, feedback, candidate selection or outcome");
        expect(window.findChild<QComboBox*>("learningCaptures")->count() == 0 &&
                   window.findChild<QPlainTextEdit*>("learningCaptureDetails")->toPlainText().isEmpty() &&
                   window.findChild<QPlainTextEdit*>("learningCaptureCorrection")->toPlainText().isEmpty() &&
                   !window.findChild<QPushButton*>("learningSaveCorrection")->isEnabled(),
            "B Skills and Learning retained A's kept exchange or correction draft");
        expect(window.findChild<QPlainTextEdit*>("developmentDetails")->toPlainText().isEmpty() &&
                   window.findChild<QLabel*>("developmentOutcome")->text().isEmpty() &&
                   !window.findChild<QPushButton*>("developmentValidate")->isEnabled(),
            "B Self Development retained A's candidate details, outcome or enabled candidate validation");
        expect(window.findChild<QLineEdit*>("recognitionWave")->text().isEmpty() &&
                   window.findChild<QLineEdit*>("audienceAlias")->text().isEmpty() &&
                   !window.findChild<QCheckBox*>("recognitionConsent")->isChecked() &&
                   !window.findChild<QPushButton*>("recognitionEnroll")->isEnabled() &&
                   window.findChild<QComboBox*>("identityEvidence")->count() == 0 &&
                   window.findChild<QPlainTextEdit*>("identityEvidenceDetails")->toPlainText().isEmpty() &&
                   window.findChild<QComboBox*>("recognitionPerson")->currentData().toString() != QString::fromStdString(person.entityId) &&
                   window.findChild<QComboBox*>("audienceRecipient")->currentData().toString() != QString::fromStdString(person.entityId) &&
                   window.findChild<QLabel*>("audienceOutcome")->text().isEmpty(),
            "B Audience and Recognition retained A's selected WAV, alias, consent, evidence, person or outcome");
        expect(QDir::currentPath() == cwd, "companion selection changed process cwd");
        expect(window.Session().Authority() == ledger, "B selection replaced the trusted authority ledger");
        exerciseAction(window.Session(), "allowed-b-fixture", true);
        // Settings stays open through the switch: the rebuilt permissions panel takes the open
        // dialog's narrow layout at once, not at the next resize.
        window.OpenSettings("Permissions");
        window.settingsDialog->resize(760, 540);
        SettleLayouts();
        window.SwitchCompanion("legacy");
        wait([&]() { return !window.switchingCompanion; });
        SettleLayouts();
        if (auto* permissions = window.settingsDialog->findChild<QScrollArea*>("settingsSection_permissions"))
            expect(permissions->horizontalScrollBar()->maximum() == 0,
                "Permissions must still fit a 760px Settings after a companion switch with Settings open");
        window.settingsDialog->hide();
        expect(!window.Session().Admits(original) && window.Session().SearchMemories("orbit", 20).size() == 1,
            "A to B to A UI lost its private mind or admitted the prior generation");
        expect(window.companionCombo->currentData().toString() == "legacy", "companion selection did not reflect actual owner");
        expect(window.Session().Authority() == ledger && !window.questions.IsAbandoned(),
            "returning A lost its authority ledger or fresh approval relay");
        exerciseAction(window.Session(), "denied-a-fixture", false);

        auto* provider = window.findChild<QComboBox*>("agentProvider");
        auto* objective = window.findChild<QPlainTextEdit*>("agentObjective");
        auto* start = window.findChild<QPushButton*>("agentStart");
        auto* repair = window.findChild<QPushButton*>("agentFixtureRepair");
        auto* retry = window.findChild<QPushButton*>("agentRetry");
        expect(provider && objective && start && repair && retry, "Studio controls are missing");
        if (!provider || !objective || !start || !repair || !retry)
            return;
        auto* hierarchy = window.findChild<QTreeWidget*>("agentHierarchy");
        auto* details = window.findChild<QLabel*>("agentDetails");
        const auto selectNode = [&](const std::string& id)
        {
            if (!hierarchy)
                return;
            for (QTreeWidgetItemIterator item(hierarchy); *item; ++item)
            {
                if ((*item)->data(0, Qt::UserRole).toString().toStdString() == id)
                {
                    hierarchy->setCurrentItem(*item);
                    return;
                }
            }
        };
        provider->setCurrentIndex(provider->findData(static_cast<int>(revia::runtime::AgentProviderMode::Demonstration)));
        objective->setPlainText("Bounded native workflow diagnostic");
        start->click();
        wait([&]() { return window.Session().AgentWorkflowSnapshot().state == WorkflowState::Paused; });
        window.agentStudioPanel->SetSnapshot(window.Session().AgentWorkflowSnapshot());
        auto* summary = window.findChild<QLabel*>("agentSummary");
        expect(summary && summary->text().contains("unavailable"), "Studio invented token telemetry");
        expect(window.Session().AgentWorkflowSnapshot().parentDecision == ParentDecision::Pending, "failed diagnostic appeared accepted");
        const auto paused = window.Session().AgentWorkflowSnapshot();
        const auto failed =
            std::find_if(paused.nodes.begin(), paused.nodes.end(), [](const auto& node) { return node.state == WorkflowState::Failed; });
        expect(failed != paused.nodes.end(), "diagnostic failure was missing from hierarchy");
        if (failed != paused.nodes.end() && !failed->attempts.empty())
        {
            selectNode(failed->id);
            expect(details && details->text().contains(QString::fromStdString(failed->attempts.back().diagnostic)),
                "selected failed node omitted its safe verification reason");
            CaptureStudioDetails(window, "failed", failures);
        }
        repair->click();
        retry->click();
        wait([&]() { return window.Session().AgentWorkflowSnapshot().state == WorkflowState::Accepted; });
        window.agentStudioPanel->SetSnapshot(window.Session().AgentWorkflowSnapshot());
        expect(window.Session().AgentWorkflowSnapshot().requests == 5, "UI recovery bypassed original workflow accounting");
        const auto accepted = window.Session().AgentWorkflowSnapshot();
        const auto reviewer = std::find_if(
            accepted.nodes.begin(), accepted.nodes.end(), [](const auto& node) { return node.role == WorkflowRole::Reviewer; });
        expect(reviewer != accepted.nodes.end() && !reviewer->attempts.empty(), "accepted reviewer evidence was missing");
        if (reviewer != accepted.nodes.end() && !reviewer->attempts.empty())
        {
            selectNode(reviewer->id);
            for (const auto& dependency : reviewer->dependencies)
                expect(details && details->text().contains(QString::fromStdString(dependency)),
                    "selected reviewer omitted an actual dependency");
            const auto& attempt = reviewer->attempts.back();
            const auto expectArtifact = [&](const ArtifactReference& artifact)
            {
                expect(details && details->text().contains(QString::fromStdString(artifact.id)) &&
                           details->text().contains("v" + QString::number(artifact.version)) &&
                           details->text().contains(QString::fromStdString(artifact.hash)),
                    "selected reviewer omitted an actual artifact id, version or hash");
            };
            for (const auto& prerequisite : attempt.prerequisites)
                expectArtifact(prerequisite);
            if (attempt.artifact)
                expectArtifact(*attempt.artifact);
            expect(details && details->text().contains("Snapshot revision: " + QString::number(accepted.sequence)) &&
                       details->text().contains("Last observed UTC:"),
                "hierarchy omitted actual snapshot revision or observation time");
        }
        {
            AgentStudioPanel observedPanel({});
            observedPanel.SetSnapshot(accepted);
            QElapsedTimer staleWait;
            staleWait.start();
            while (staleWait.elapsed() < 2300)
            {
                QCoreApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            auto* observedDetails = observedPanel.findChild<QLabel*>("agentDetails");
            expect(observedDetails && observedDetails->text().contains("Snapshot stale"),
                "a hierarchy without fresh owner observations invented current status");
        }
        const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
        auto* studioScroll = window.findChild<QScrollArea*>("agentStudioScroll");
        revia::desktop::SelectNavigationPage(studioScroll);
        for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
        {
            if (studioScroll)
                studioScroll->window()->resize(size);
            window.agentStudioPanel->SetSnapshot(window.Session().AgentWorkflowSnapshot());
            SettleLayouts();
            auto* scroll = window.findChild<QScrollArea*>("agentStudioScroll");
            expect(scroll && scroll->horizontalScrollBar()->maximum() == 0, "live Agent Studio overflows horizontally");
            if (scroll)
                scroll->verticalScrollBar()->setValue(0);
            SettleLayouts();
            if (!renderDirectory.isEmpty() && scroll)
            {
                expect(scroll->window()->grab().save(
                           QDir(renderDirectory).filePath(QString("agent-studio-accepted-%1x%2.png").arg(size.width()).arg(size.height()))),
                    "live Agent Studio capture failed");
            }
        }
        CaptureStudioDetails(window, "evidence", failures);
    }

    static void SettleLayouts()
    {
        QCoreApplication::processEvents();
        // Tab changes post layout requests before the scroll range is current.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    }

    static void RunHealthUiAndLayout(ReviaWindow& window, int& failures)
    {
        using revia::runtime::RuntimeEvent;
        using revia::runtime::RuntimeEventKind;
        const auto expect = [&](const bool condition, const QString& message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message.toStdString() << '\n';
                ++failures;
            }
        };
        const auto health = [&](const std::string& phase)
        {
            RuntimeEvent event;
            event.kind = RuntimeEventKind::ComponentStatus;
            event.component = "Voice health";
            event.phase = phase;
            event.message = "private-worker-body-must-not-reach-health-view";
            window.Session().Events().Publish(event);
            QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
            QCoreApplication::processEvents();
        };
        window.show();
        auto* sidebar = window.findChild<QWidget*>("navigationSidebar");
        expect(sidebar != nullptr && window.findChildren<QTabWidget*>("tabs").isEmpty(),
            "main destinations must use the sidebar and panel windows instead of a tab row");
        // Every panel has its menu entry, opens as its own window with its page showing, and the
        // entry follows the window.
        for (const revia::desktop::PanelSpec& spec : revia::desktop::PanelCatalog())
        {
            QAction* action = window.panelHost->Action(spec.id);
            expect(action != nullptr && window.sidebarPanelsMenu->actions().contains(action),
                QString("the sidebar menu's Panels submenu must list the %1 panel").arg(spec.title));
            if (!action)
                continue;
            action->trigger();
            SettleLayouts();
            auto* panel = window.panelHost->Window(spec.id);
            expect(panel && panel->isVisible() && panel->Page() && panel->Page()->isVisible() && action->isChecked(),
                QString("the %1 menu entry must open its window with the page showing").arg(spec.title));
            window.panelHost->Close(spec.id);
            expect(!action->isChecked(), QString("the %1 menu entry must uncheck when its window closes").arg(spec.title));
        }
        window.SetNavigationLocked(true);
        expect(!window.navigationSidebar->isEnabled() && !window.chatColumn->isEnabled() && !window.settingsDialog->isEnabled(),
            "the companion replacement lock must reach the sidebar, the chat and Settings");
        window.SetNavigationLocked(false);
        expect(window.navigationSidebar->isEnabled() && window.chatColumn->isEnabled() && window.settingsDialog->isEnabled(),
            "navigation must recover when it is admitted again");
        window.AppendActivity("Sidebar warning count fixture", ReviaWindow::ActivitySeverity::Warning);
        if (auto* activity = window.panelHost->Action(revia::desktop::PanelId::Activity))
            expect(activity->text().contains('(') && window.panelHost->Window(revia::desktop::PanelId::Activity)->windowTitle().contains('('),
                "the Activity entry and window title must carry the actual warning count");
        window.panelHost->CloseAll();
        window.UpdateState(revia::runtime::RuntimeState::Error, "The language model is unavailable; typed actions remain available.");
        auto* runtimeDetails = window.findChild<QPushButton*>("runtimeDetailsButton");
        expect(runtimeDetails != nullptr && !runtimeDetails->isHidden(), "model startup errors must offer an actionable diagnostics route");
        if (runtimeDetails)
        {
            runtimeDetails->click();
            expect(window.findChild<QWidget*>("activityPage")->isVisible(), "error Details must open the existing Runtime Activity page");
            window.UpdateState(revia::runtime::RuntimeState::Idle, "Ready");
            expect(runtimeDetails->isHidden(), "startup Details must leave the header once the error clears");
            window.panelHost->CloseAll();
        }
        health("Degraded");
        auto* chatBadge = window.findChild<QLabel*>("chatVoiceHealthBadge");
        auto* voiceBadge = window.findChild<QLabel*>("voiceHealthBadge");
        auto* voiceDetail = window.findChild<QLabel*>("voiceHealthDetail");
        expect(chatBadge != nullptr && voiceBadge != nullptr && voiceDetail != nullptr,
            "current synthesis health must be visible in Chat and Voice, beyond the activity log");
        if (chatBadge && voiceBadge && voiceDetail)
        {
            expect(chatBadge->text() == voiceBadge->text() && chatBadge->text().contains("Text mode"),
                "both views must react to an actual queued health event");
            expect(voiceDetail->text().contains("unknown") && !voiceDetail->text().contains("private-worker"),
                "fault detail must state unknown cause without copying a worker payload");
            RuntimeEvent ready;
            ready.kind = RuntimeEventKind::ComponentStatus;
            ready.component = "Voice";
            ready.phase = "Ready";
            window.HandleRuntimeEvent(ready);
            expect(chatBadge->text().contains("Text mode"), "generic Voice Ready must not clear the health card");
            health("Available");
            expect(chatBadge->text().contains("Synthesis ready") && voiceDetail->text().contains("playback"),
                "fresh generation must update both views without claiming speaker verification");
            health("Disabled");
            expect(chatBadge->text().contains("Voice off"), "mute must have a distinct current-health presentation");
            health("Unverified");
            expect(chatBadge->text().contains("Unverified"), "selection reset must not retain verified availability");
            health("Degraded");
            health("UnexpectedWorkerPhase");
            expect(chatBadge->text().contains("Text mode"), "unknown health phases must preserve the current safe health");
            RuntimeEvent cue;
            cue.kind = RuntimeEventKind::ComponentStatus;
            cue.component = "System cues";
            cue.phase = "Prepared";
            cue.message = "private-worker-body-must-not-reach-health-view";
            window.Session().Events().Publish(cue);
            QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
            auto* cueLabel = window.findChild<QLabel*>("voiceCueStatus");
            expect(cueLabel && cueLabel->text().contains("validated") && !cueLabel->text().contains("private-worker"),
                "cached status must use safe presentation from the existing event bus");
            expect(chatBadge->text().contains("Text mode"), "cache preparation must not prove fault recovery");
            bool rawPayloadVisible = false;
            for (auto* table : window.findChildren<QTableWidget*>())
            {
                for (int row = 0; row < table->rowCount(); ++row)
                {
                    for (int column = 0; column < table->columnCount(); ++column)
                    {
                        const auto* item = table->item(row, column);
                        if (item && (item->text().contains("private-worker-body") || item->toolTip().contains("private-worker-body")))
                            rawPayloadVisible = true;
                    }
                }
            }
            expect(!rawPayloadVisible, "raw health and cue payloads must not leak through generic Runtime tables");
            auto* details = window.findChild<QPushButton*>("voiceHealthDetailsButton");
            expect(details != nullptr, "Chat must offer a visible path to detailed health");
            if (details)
            {
                details->click();
                QCoreApplication::processEvents();
                expect(window.panelHost->IsOpen(revia::desktop::PanelId::Voice) && window.findChild<QWidget*>("voiceTab")->isVisible(),
                    "Details must open the Voice window and its existing view");
            }
        }
        const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
        if (!renderDirectory.isEmpty())
            expect(QDir().mkpath(renderDirectory), "render directory must be writable");
        QList<QWidget*> pages = window.NavigationPages();
        for (QWidget* page : pages)
            page->setProperty("captureTitle", window.NavigationTitle(page));
        expect(pages.size() == 30, "every destination, including Mind's four views, must remain reachable");
        for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
        {
            window.resize(size);
            SettleLayouts();
            expect(window.size() == size, QString("the full interface must fit %1x%2").arg(size.width()).arg(size.height()));
            if (sidebar && size.width() == 760)
            {
                expect(sidebar->isVisible() && sidebar->width() <= 64, "small windows must retain a compact navigation rail");
                for (auto* button : sidebar->findChildren<QToolButton*>())
                {
                    if (!button->isVisible())
                        continue;
                    expect(!button->accessibleName().isEmpty() && !button->toolTip().isEmpty(),
                        QString("the rail's %1 must keep an accessible name and a readable tooltip").arg(button->objectName()));
                }
            }
            if (!renderDirectory.isEmpty())
            {
                const auto previousState = window.lastRuntimeState;
                const auto previousDetail = window.lastRuntimeDetail;
                window.UpdateState(revia::runtime::RuntimeState::Error, "The language model is unavailable; typed actions remain available.");
                revia::desktop::SelectNavigationPage(window.findChild<QWidget*>("chatPage"));
                SettleLayouts();
                expect(runtimeDetails && runtimeDetails->isVisible() && window.size() == size,
                    "model error and diagnostics must fit each requested viewport");
                expect(window.grab().save(QDir(renderDirectory).filePath(QString("startup-error-%1x%2.png").arg(size.width()).arg(size.height()))),
                    "startup error must have a fresh Qt render");
                window.UpdateState(previousState, previousDetail);
            }
            if (sidebar && size.width() == 1040)
            {
                auto* toggle = sidebar->findChild<QToolButton*>("sidebarToggle");
                expect(toggle != nullptr, "sidebar must offer a collapse control");
                if (toggle)
                {
                    const int expandedWidth = sidebar->width();
                    const int contentWidth = window.chatColumn->width();
                    toggle->click();
                    SettleLayouts();
                    expect(sidebar->width() < expandedWidth && window.chatColumn->width() > contentWidth,
                        "collapsing the sidebar must give space back to the content");
                    toggle->click();
                    SettleLayouts();
                    expect(sidebar->width() == expandedWidth, "expanding the sidebar must restore its readable labels");
                }
            }
            for (auto* group : window.findChildren<QTabWidget*>())
            {
                auto* bar = group->tabBar();
                expect(!bar->usesScrollButtons(), "navigation must not depend on left/right scroll arrows");
            }
            for (QWidget* page : pages)
            {
                revia::desktop::SelectNavigationPage(page);
                QWidget* host = page->window();
                if (host != &window)
                    host->resize(size);
                SettleLayouts();
                expect(page->isVisible() && host->size() == size,
                    QString("%1 must show at %2x%3 through its full navigation route")
                        .arg(page->property("captureTitle").toString())
                        .arg(size.width())
                        .arg(size.height()));
                if (const auto panel = window.panelHost->IdOf(page))
                {
                    QAction* action = window.panelHost->Action(*panel);
                    expect(action && action->isChecked(), "deep-link page selection must check the panel's menu entry");
                }
                for (auto* group : window.findChildren<QTabWidget*>())
                {
                    auto* bar = group->tabBar();
                    if (!bar->isVisible())
                        continue;
                    for (int index = 0; index < bar->count(); ++index)
                        expect(bar->rect().contains(bar->tabRect(index)), "every navigation label must fit without clipping");
                }
                auto scrolls = page->findChildren<QScrollArea*>();
                if (auto* scroll = qobject_cast<QScrollArea*>(page))
                    scrolls.push_back(scroll);
                for (auto* scroll : scrolls)
                {
                    if (!scroll->isVisible())
                        continue;
                    expect(
                        scroll->horizontalScrollBar()->maximum() == 0, QString("%1 at %2x%3 must fit horizontally without hidden overflow")
                                                                           .arg(page->objectName())
                                                                           .arg(size.width())
                                                                           .arg(size.height()));
                }
                if (!renderDirectory.isEmpty())
                {
                    const QString name = (page->objectName().isEmpty() ? page->property("captureTitle").toString() : page->objectName())
                                             .toLower()
                                             .replace(QRegularExpression("[^a-z0-9_]+"), "-");
                    const QString path =
                        QDir(renderDirectory).filePath(QString("%1-%2x%3.png").arg(name).arg(size.width()).arg(size.height()));
                    expect(host->grab().save(path), "actual Qt render must be saved");
                    for (const auto& anchor : {QString("processApprovedExecutables"), QString("browserApprovedOrigins")})
                    {
                        auto* field = page->findChild<QPlainTextEdit*>(anchor);
                        if (!field)
                            continue;
                        for (auto* scroll : scrolls)
                        {
                            if (!scroll->isAncestorOf(field))
                                continue;
                            scroll->ensureWidgetVisible(field, 0, 100);
                            SettleLayouts();
                            expect(host->grab().save(QDir(renderDirectory)
                                           .filePath(QString("%1-%2x%3.png").arg(anchor).arg(size.width()).arg(size.height()))),
                                "command and browser controls must render in the responsive viewport");
                        }
                    }
                }
                if (host != &window)
                    host->hide();
            }
        }
        window.panelHost->CloseAll();
        if (!renderDirectory.isEmpty())
        {
            window.resize(1040, 720);
            health("Available");
            expect(window.grab().save(QDir(renderDirectory).filePath("chat-recovered-1040x720.png")),
                "fresh recovery must have an actual Qt render");
            health("Disabled");
            expect(window.grab().save(QDir(renderDirectory).filePath("chat-disabled-1040x720.png")),
                "disabled voice must have an actual Qt render");
        }
    }

    static void RunConversationQualityUi(ReviaWindow& window, int& failures)
    {
        using namespace revia::runtime;
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        auto* status = window.findChild<QLabel*>("conversationCurrentStatus");
        auto* details = window.findChild<QWidget*>("conversationStatusDetails");
        auto* timing = window.findChild<QLabel*>("conversationResponseTiming");
        auto* quality = window.findChild<QLabel*>("conversationQualityDiagnostic");
        auto* review = window.findChild<QPushButton*>("reviewAnswerButton");
        expect(status && details && timing && quality && review,
            "Chat must project current work, timing, diagnostic quality and explicit answer review");
        if (!status || !details || !timing || !quality || !review)
            return;
        expect(details->isHidden(), "conversation advanced details must start collapsed");
        RuntimeEvent event{RuntimeEventKind::StateChanged, RuntimeState::Thinking, "Working on the current request"};
        window.HandleRuntimeEvent(event);
        expect(status->text().contains("Thinking"), "current conversation card must consume admitted runtime state");
        event.kind = RuntimeEventKind::ComponentStatus;
        event.component = "Response timing";
        event.message = "Accepted input to admitted text: 1.4 s; first audio unavailable";
        window.HandleRuntimeEvent(event);
        expect(timing->text().contains("1.4 s") && timing->text().contains("unavailable"),
            "response timing must retain measured stages and unavailable stages");
        event.component = "Conversation quality";
        event.phase = "Flagged";
        event.message = "Possible coverage issue";
        window.HandleRuntimeEvent(event);
        expect(quality->text().contains("Diagnostic") && quality->text().contains("coverage"),
            "monitor flags must remain explicitly diagnostic in Chat");
        event.kind = RuntimeEventKind::AssistantMessage;
        event.audienceRevision = window.Session().Audience().revision;
        event.message = "Actual answer for review.";
        event.component.clear();
        event.turnId = 940;
        window.HandleRuntimeEvent(event);
        expect(review->isEnabled(), "a displayed assistant answer must enable deliberate review");
        event.kind = RuntimeEventKind::Memory;
        event.message = "Memory summary must never replace the review target.";
        window.HandleRuntimeEvent(event);
        review->click();
        auto* captured = window.findChild<QPlainTextEdit*>("answerFeedbackTarget");
        expect(captured && captured->toPlainText() == "Actual answer for review.",
            "answer review must capture the actual displayed assistant answer, excluding memory entries");
        if (window.answerFeedbackDialog)
        {
            revia::learning::QualityFeedback feedback;
            std::string error;
            expect(!window.answerFeedbackDialog->CaptureFeedback(feedback, error),
                "a diagnostic flag must not supply an owner criterion or judgment");
            auto* keep = window.findChild<QCheckBox*>("answerFeedbackKeepExchange");
            expect(keep && !keep->isChecked() && !window.answerFeedbackDialog->KeepExchange(),
                "keeping a judged reply's exact exchange must be the owner's explicit choice, off by default");
            window.findChild<QLineEdit*>("answerFeedbackCriterion")->setText("Answer the requested fact.");
            window.findChild<QPlainTextEdit*>("answerFeedbackEvidence")->setPlainText("The reply omitted the requested fact.");
            expect(window.answerFeedbackDialog->CaptureFeedback(feedback, error) && feedback.criterion == "Answer the requested fact." &&
                       feedback.evidence == "The reply omitted the requested fact." && !feedback.criterionSatisfied &&
                       feedback.source == revia::learning::QualityEvidenceSource::OwnerJudgment &&
                       feedback.origin.SameSession(window.Session().Stamp()) &&
                       feedback.audienceRevision == window.Session().Audience().revision,
                "explicit answer judgment must retain the owner's criterion, evidence and captured origin");
            const auto renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
            if (!renderDirectory.isEmpty())
            {
                SettleLayouts();
                expect(window.answerFeedbackDialog->grab().save(QDir(renderDirectory).filePath("answer-feedback-dialog.png")),
                    "the explicit owner answer judgment dialog must have a fresh Qt capture");
            }
        }
        if (auto* dialog = captured ? captured->window() : nullptr)
            dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        RuntimeEvent firstFragment{RuntimeEventKind::ReplyFragment, RuntimeState::Thinking, "First displayed sentence."};
        firstFragment.stamp = window.Session().Stamp();
        firstFragment.audienceRevision = window.Session().Audience().revision;
        firstFragment.conversationTurnId = 941;
        firstFragment.turnId = 9411;
        window.HandleRuntimeEvent(firstFragment);
        auto secondFragment = firstFragment;
        secondFragment.message = "Second displayed sentence.";
        secondFragment.turnId = 9412;
        window.HandleRuntimeEvent(secondFragment);
        expect(window.latestDisplayedAnswer == "First displayed sentence.\n\nSecond displayed sentence.",
            "distinct speech utterances from one conversation turn must retain the complete displayed answer");
        event.stamp = window.Session().Stamp();
        event.stamp.generation += 1;
        event.kind = RuntimeEventKind::StateChanged;
        event.message = "stale-status-sentinel";
        window.HandleRuntimeEvent(event);
        expect(!status->text().contains("stale-status-sentinel"), "retired-origin events must not reach the card");
        const auto beforeAudienceQueue = window.chatEntries.size();
        const auto beforeQueuedState = window.lastRuntimeState;
        const auto beforeQueuedStateDetail = window.lastRuntimeDetail;
        const auto beforeQueuedStatus = status->text();
        RuntimeEvent missingAudience{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "missing-audience-answer-sentinel"};
        missingAudience.stamp = window.Session().Stamp();
        window.HandleRuntimeEvent(missingAudience);
        expect(window.chatEntries.size() == beforeAudienceQueue,
            "an assistant reply without captured audience metadata must not become a review target");
        RuntimeEvent delayed{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "queued-private-answer-sentinel"};
        delayed.stamp = window.Session().Stamp();
        delayed.audienceRevision = window.Session().Audience().revision;
        window.Session().Events().Publish(delayed);
        RuntimeEvent delayedFindings{
            RuntimeEventKind::InvestigationFindings, RuntimeState::Thinking, "Native file check read queued-private-findings-sentinel"};
        delayedFindings.stamp = window.Session().Stamp();
        delayedFindings.audienceRevision = window.Session().Audience().revision;
        delayedFindings.detail = "Private native file contents: queued-private-findings-sentinel";
        window.Session().Events().Publish(delayedFindings);
        RuntimeEvent delayedProposal{RuntimeEventKind::Proposal, RuntimeState::Idle, "queued-private-proposal-sentinel"};
        delayedProposal.stamp = window.Session().Stamp();
        delayedProposal.audienceRevision = window.Session().Audience().revision;
        delayedProposal.detail = "Private proposal evidence: queued-private-proposal-sentinel";
        window.Session().Events().Publish(delayedProposal);
        RuntimeEvent delayedComponent{
            RuntimeEventKind::ComponentStatus, RuntimeState::Thinking, "Private background task detail: queued-private-component-sentinel"};
        delayedComponent.stamp = window.Session().Stamp();
        delayedComponent.audienceRevision = window.Session().Audience().revision;
        delayedComponent.component = "Conversation quality";
        delayedComponent.phase = "Flagged";
        delayedComponent.detail = "Private task evidence: queued-private-component-sentinel";
        window.Session().Events().Publish(delayedComponent);
        RuntimeEvent delayedState{
            RuntimeEventKind::StateChanged, RuntimeState::WaitingForConfirmation, "Private task summary: queued-private-state-sentinel"};
        delayedState.stamp = window.Session().Stamp();
        delayedState.audienceRevision = window.Session().Audience().revision;
        window.Session().Events().Publish(delayedState);
        SessionResult delayedSystem;
        delayedSystem.stamp = window.Session().Stamp();
        delayedSystem.audienceRevision = window.Session().Audience().revision;
        delayedSystem.succeeded = true;
        delayedSystem.fromAssistant = false;
        delayedSystem.text = "queued-private-system-result-sentinel";
        QMetaObject::invokeMethod(
            &window, [&window, delayedSystem]() { window.PresentSessionResult(delayedSystem); }, Qt::QueuedConnection);
        auto delayedSystemFailure = delayedSystem;
        delayedSystemFailure.succeeded = false;
        delayedSystemFailure.text = "queued-private-system-failure-sentinel";
        delayedSystemFailure.reason = "Private action failure: queued-private-system-failure-sentinel";
        QMetaObject::invokeMethod(
            &window, [&window, delayedSystemFailure]() { window.PresentSessionResult(delayedSystemFailure); }, Qt::QueuedConnection);
        std::string audienceError;
        expect(window.Session().SetAudience({revia::identity::AudienceKind::Public, "ui-public", 0, {}}, audienceError) &&
                   window.Session().SetAudience({revia::identity::AudienceKind::Private, "ui-private", 0, {}}, audienceError),
            "the queued audience regression must complete its public-to-private transition");
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        expect(window.chatEntries.size() == beforeAudienceQueue && !window.latestDisplayedAnswer.contains("queued-private-answer-sentinel"),
            "queued reply text must not adopt the newer audience or become a displayed review target");
        auto* resultEvidence = window.findChild<QLabel*>("conversationResultEvidence");
        expect(resultEvidence && !resultEvidence->text().contains("queued-private-findings-sentinel"),
            "queued private native findings must not reach the transcript or conversation evidence under a newer audience");
        expect(resultEvidence && !resultEvidence->text().contains("queued-private-proposal-sentinel"),
            "all events with captured stale audience metadata must be refused before any projection");
        expect(!quality->text().contains("queued-private-component-sentinel") && window.lastRuntimeState == beforeQueuedState &&
                   window.lastRuntimeDetail == beforeQueuedStateDetail && status->text() == beforeQueuedStatus,
            "queued private component detail and task state must not change conversation or status projection after an audience epoch");
        expect(std::none_of(window.activityEntries.begin(), window.activityEntries.end(),
                   [](const auto& entry)
                   {
                       return entry.message.contains("queued-private-component-sentinel") ||
                              entry.message.contains("queued-private-state-sentinel") ||
                              entry.message.contains("queued-private-system-failure-sentinel");
                   }),
            "queued private component detail and task summary must not enter the activity projection after an audience epoch");
        auto currentSystem = delayedSystem;
        currentSystem.audienceRevision = window.Session().Audience().revision;
        currentSystem.text = "Current admitted system result positive control.";
        window.PresentSessionResult(currentSystem);
        expect(window.chatEntries.size() == beforeAudienceQueue + 1 && window.chatEntries.back().speaker == "System" &&
                   window.chatEntries.back().body == "Current admitted system result positive control.",
            "a system action result with current captured audience metadata must remain visible");
        auto currentSystemFailure = currentSystem;
        currentSystemFailure.succeeded = false;
        currentSystemFailure.text = "Current admitted system failure positive control.";
        currentSystemFailure.reason = "Current admitted action failure evidence.";
        window.PresentSessionResult(currentSystemFailure);
        expect(window.chatEntries.size() == beforeAudienceQueue + 2 &&
                   window.chatEntries.back().body == currentSystemFailure.text.c_str() &&
                   std::any_of(window.activityEntries.begin(), window.activityEntries.end(),
                       [](const auto& entry) { return entry.message.contains("Current admitted action failure evidence."); }),
            "a current system failure must retain its displayed text and admitted activity evidence");
        review->click();
        expect(!review->isEnabled() && !window.answerFeedbackDialog,
            "an older displayed reply must not be reviewed under a newer audience revision");
        auto* ownerOutcome = window.findChild<QLabel*>("conversationOwnerOutcome");
        expect(ownerOutcome && ownerOutcome->text().contains("earlier audience"),
            "refusing an older answer target must explain which displayed selection is stale");
        RuntimeEvent fresh{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "Fresh admitted reply for the next fixture."};
        fresh.stamp = window.Session().Stamp();
        fresh.audienceRevision = window.Session().Audience().revision;
        window.HandleRuntimeEvent(fresh);
        expect(review->isEnabled(), "fresh captured audience metadata must restore deliberate answer review");
        expect(ownerOutcome && ownerOutcome->text().isEmpty(),
            "a fresh admitted answer must clear the obsolete earlier-audience review warning");
        RuntimeEvent pending{RuntimeEventKind::StateChanged, RuntimeState::WaitingForConfirmation, "An action awaits its owner."};
        pending.stamp = window.Session().Stamp();
        pending.audienceRevision = window.Session().Audience().revision;
        window.HandleRuntimeEvent(pending);
        fresh.message = "A fresh answer while the owner action remains pending.";
        window.HandleRuntimeEvent(fresh);
        expect(ownerOutcome && ownerOutcome->text() == "An action is waiting for your decision.",
            "fresh answer capture must preserve a separate pending owner action");
        pending.state = RuntimeState::Thinking;
        pending.message = "Working on the current request";
        window.HandleRuntimeEvent(pending);
        const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
        if (!renderDirectory.isEmpty())
        {
            window.show();
            for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
            {
                window.resize(size);
                auto* expand = window.findChild<QPushButton*>("conversationDetailsButton");
                expand->setChecked(false);
                SettleLayouts();
                expect(window.grab().save(
                           QDir(renderDirectory).filePath(QString("conversation-status-%1x%2.png").arg(size.width()).arg(size.height()))),
                    "compact conversation status must have a fresh Qt capture");
                expand->setChecked(true);
                SettleLayouts();
                expect(window.size() == size, "expanded conversation details must preserve the requested window size");
                auto* detailScroll = window.findChild<QScrollArea*>("conversationDetailsScroll");
                expect(detailScroll && detailScroll->viewport()->height() >= timing->fontMetrics().lineSpacing() * 3,
                    "expanded conversation diagnostics must retain a readable scroll viewport");
                expect(
                    status->height() >= status->fontMetrics().lineSpacing() &&
                        window.chatHistory->visibleRegion().boundingRect().height() >= window.chatHistory->fontMetrics().lineSpacing() * 3,
                    "expanded conversation status must retain readable status and visible transcript lines");
                expect(window.grab().save(
                           QDir(renderDirectory).filePath(QString("conversation-details-%1x%2.png").arg(size.width()).arg(size.height()))),
                    "expanded conversation evidence must have a fresh Qt capture");
                expand->setChecked(false);
            }
        }
    }

    static void RunMemoryRevisionUi(ReviaWindow& window, int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        memoryDecision original;
        original.bSuccess = true;
        original.bShouldRemember = true;
        original.category = "fact";
        original.summary = "UI exact revision original sentinel";
        longTermMemory store(window.Session().Paths().Resolve("Memory/revia_memory.db").string());
        bool added = false;
        expect(store.Save(original, added), "UI revision fixture must persist a selected original record");
        window.memoryPanel->Refresh();
        auto* table = window.findChild<QTableWidget*>("memoryRecords");
        auto* revise = window.findChild<QPushButton*>("memoryReviseSelected");
        expect(table && revise, "Memory must expose a deliberate selected-record revision action");
        if (!table || !revise)
            return;
        const auto entries = window.Session().Memories();
        const auto found = std::find_if(entries.begin(), entries.end(),
            [](const memoryEntry& entry) { return entry.summary == "UI exact revision original sentinel"; });
        expect(found != entries.end(), "the selected revision target must come from the actual memory store");
        if (found == entries.end())
            return;
        revia::memory::MemoryRevisionRequest captured;
        bool requested = false;
        window.memoryPanel->SetRevisionSubmitter(
            [&](const revia::memory::MemoryRevisionRequest& request)
            {
                captured = request;
                requested = true;
                return true;
            });
        for (int row = 0; row < table->rowCount(); ++row)
        {
            if (table->item(row, 0)->text() == "UI exact revision original sentinel")
                table->selectRow(row);
        }
        expect(revise->isEnabled(), "a current durable record must permit deliberate correction");
        revise->click();
        auto* corrected = window.findChild<QPlainTextEdit*>("memoryRevisionCorrected");
        auto* record = window.findChild<QPushButton*>("memoryRevisionRecord");
        expect(corrected && record, "the correction dialog must require explicit owner text");
        if (corrected && record)
        {
            record->click();
            expect(!requested, "empty owner correction must never reach the session boundary");
            corrected->setPlainText("UI exact corrected fact");
            window.findChild<QLineEdit*>("memoryRevisionReason")->setText("I explicitly correct the selected fact.");
            window.findChild<QPlainTextEdit*>("memoryRevisionEvidence")->setPlainText("Selected record has the outdated value.");
            const auto renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
            if (!renderDirectory.isEmpty())
            {
                SettleLayouts();
                expect(corrected->window()->grab().save(QDir(renderDirectory).filePath("memory-revision-dialog.png")),
                    "the explicit owner memory correction dialog must have a fresh Qt capture");
            }
            record->click();
            expect(requested && captured.originalId == found->id &&
                       captured.expectedSummaryDigest == "3b5e413b7bfb3cda01061a92e1cb3cb47c220abfceb4983dcffde5bb6cfbfc86" &&
                       captured.corrected.summary == "UI exact corrected fact" && captured.corrected.category == "fact" &&
                       captured.priorReceiptId.empty() && !captured.ownerRequestId.empty() && captured.corrected.embedding.empty() &&
                       captured.origin.SameSession(window.Session().Stamp()) &&
                       captured.audienceRevision == window.Session().Audience().revision,
                "owner correction must carry the exact selected durable ID, prior digest, category and captured origin");
            expect(store.Load().size() == entries.size(), "capturing a correction request must not rewrite the selected row");
            revia::memory::MemoryRevisionReceipt receipt;
            std::string error;
            expect(store.SaveOwnerRevision(captured, receipt, error), "the controlled native owner revision must persist");
            window.memoryPanel->Refresh();
            bool historicalVisible = false;
            for (int row = 0; row < table->rowCount(); ++row)
            {
                if (table->item(row, 0)->text() == "UI exact revision original sentinel")
                {
                    table->selectRow(row);
                    historicalVisible = table->item(row, 5)->text() == "Historical";
                }
            }
            expect(historicalVisible && !revise->isEnabled(),
                "a replaced record must remain visible while refusing another correction to its historical row");
            auto* provenance = window.findChild<QLabel*>("memoryRevisionProvenance");
            expect(provenance && provenance->text().contains(QString::fromStdString(receipt.revisedId)),
                "the original row must expose its exact current replacement");
            if (provenance)
                QMetaObject::invokeMethod(provenance, "linkActivated", Q_ARG(QString, QString("current")));
            expect(
                revise->isEnabled() && table->currentRow() >= 0 && table->item(table->currentRow(), 0)->text() == "UI exact corrected fact",
                "the current-replacement link must select the actual revised row");
            corrected->window()->close();
            if (!renderDirectory.isEmpty())
            {
                QWidget* memoryTab = window.findChild<QWidget*>("memoryTab");
                revia::desktop::SelectNavigationPage(memoryTab);
                for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
                {
                    memoryTab->window()->resize(size);
                    SettleLayouts();
                    expect(memoryTab->window()->size() == size, "memory revision provenance must fit the requested window size");
                    auto* memoryScroll = window.findChild<QScrollArea*>("memoryPageScroll");
                    memoryScroll->verticalScrollBar()->setValue(table->mapTo(memoryScroll->widget(), QPoint(0, 0)).y());
                    SettleLayouts();
                    if (table->viewport()->visibleRegion().boundingRect().height() < table->rowHeight(0) + table->rowHeight(1))
                        std::cerr << "Memory viewport: visible=" << table->viewport()->visibleRegion().boundingRect().height()
                                  << " required=" << table->rowHeight(0) + table->rowHeight(1)
                                  << " scroll=" << memoryScroll->verticalScrollBar()->value()
                                  << '/' << memoryScroll->verticalScrollBar()->maximum()
                                  << " inner=" << memoryScroll->widget()->height() << " table=" << table->height() << '\n';
                    expect(table->viewport()->visibleRegion().boundingRect().height() >= table->rowHeight(0) + table->rowHeight(1),
                        "memory revision history must retain space for the original and current rows");
                    expect(memoryTab->window()->grab().save(
                               QDir(renderDirectory).filePath(QString("memory-revision-%1x%2.png").arg(size.width()).arg(size.height()))),
                        "actual historical and current memory rows must have fresh Qt captures");
                }
            }
        }
        window.ConfigureMemoryRevisionControls();
    }

    // The redesigned shell on the live window (design spec 8.4): its widgets exist, the palette,
    // Settings, the toast and the theme reach what they should, and the composer, transcript and
    // header behave. Only display-side setters are used; no Preference, settings-file,
    // capability or workspace row is written.
    static void RunShellUi(ReviaWindow& window, int& failures)
    {
        using namespace revia::runtime;
        namespace names = revia::desktop::names;
        using revia::desktop::PanelId;
        const auto expect = [&](const bool condition, const QString& message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message.toStdString() << '\n';
                ++failures;
            }
        };
        window.resize(1040, 720);
        window.show();
        window.panelHost->CloseAll();
        SettleLayouts();

        for (const auto& list : {std::vector<names::Name>(names::kStyled.begin(), names::kStyled.end()),
                 std::vector<names::Name>(names::kUnstyled.begin(), names::kUnstyled.end())})
        {
            for (const names::Name& name : list)
            {
                const QString objectName = QString::fromUtf8(name.name.data(), static_cast<qsizetype>(name.name.size()));
                if (name.alwaysPresent)
                    expect(window.findChild<QObject*>(objectName) != nullptr, QString("%1 must exist under the window").arg(objectName));
            }
        }

        // The sidebar's chrome is the same on first show as after a collapse and expand: the window's
        // stylesheet must not size its buttons over what the sidebar sets.
        {
            const auto chrome = [&window]()
            {
                QList<QRect> geometry;
                for (const char* name : {"sidebarToggle", "sidebarSettingsButton", "sidebarMenuButton", "sidebarFooter"})
                {
                    auto* widget = window.navigationSidebar->findChild<QWidget*>(name);
                    geometry.push_back(widget != nullptr ? QRect(widget->mapTo(window.navigationSidebar, QPoint(0, 0)), widget->size()) : QRect());
                }
                return geometry;
            };
            const QList<QRect> before = chrome();
            expect(before[0].size() == QSize(32, 32) && before[1].size() == QSize(32, 32) && before[2].height() == 44,
                "the sidebar's header and footer buttons must be 32px and the account button 44px on first show");
            window.navigationSidebar->ToggleButton()->click();
            SettleLayouts();
            window.navigationSidebar->ToggleButton()->click();
            SettleLayouts();
            expect(chrome() == before, "a collapse and expand must leave the sidebar's buttons where they were");
        }

        // The palette finds a panel by a few letters and opens it with Enter.
        window.commandPalette->Open("mem");
        SettleLayouts();
        auto* paletteInput = window.findChild<QLineEdit*>("commandPaletteInput");
        expect(window.commandPalette->IsOpen() && paletteInput != nullptr, "Ctrl+K's palette must open with its input");
        if (paletteInput)
        {
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(paletteInput, &enter);
            SettleLayouts();
        }
        expect(window.panelHost->IsOpen(PanelId::Memory) && !window.commandPalette->IsOpen(), "\"mem\" and Enter must open the Memory window");
        window.panelHost->CloseAll();

        window.OpenSettings("Voice");
        SettleLayouts();
        auto* stack = window.settingsDialog->findChild<QStackedWidget*>("settingsStack");
        expect(window.settingsDialog->isVisible() && stack &&
                   stack->currentWidget() == window.settingsDialog->SectionPage(revia::desktop::SettingsSection::Voice),
            "OpenSettings(\"Voice\") must show Settings on its Voice section");
        window.settingsDialog->hide();
        window.settingsDialog->SetValue("speechRecognition.handsFree", true);
        expect(window.autoSendVoiceCheck && !window.autoSendVoiceCheck->isEnabled(), "hands-free listening must disable sending speech automatically");
        window.settingsDialog->SetValue("speechRecognition.handsFree", false);

        window.statusBar()->showMessage("toast fixture");
        SettleLayouts();
        auto* toastLabel = window.findChild<QLabel*>("toastLabel");
        expect(toastLabel && toastLabel->isVisible() && toastLabel->text() == "toast fixture" &&
                   window.statusBar()->currentMessage() == "toast fixture",
            "a status message must show as the toast and stay the status bar's message");
        window.statusBar()->clearMessage();

        // The transcript follows the theme: Markdown and classes are rendered again in its colours.
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.AppendChat(QString::fromStdString(window.Session().DisplayName()), "A themed reply.");
        revia::desktop::theme::SetPreference(revia::desktop::theme::Preference::Dark);
        expect(window.chatHistory->toHtml().contains(revia::desktop::theme::Dark().text.name(), Qt::CaseInsensitive),
            "the dark scheme must reach the transcript's text colour");
        revia::desktop::theme::SetPreference(revia::desktop::theme::Preference::Light);
        expect(window.chatHistory->toHtml().contains(revia::desktop::theme::Light().text.name(), Qt::CaseInsensitive),
            "the light scheme must come back to the transcript");

        // What Revia is checking can be opened while the turn that checks it is still running.
        {
            window.chatEntries.clear();
            window.ResetConversationPresentation();
            window.AppendChat("You", "Fixture question", true);
            ReviaWindow::ChatEntry work;
            work.kind = ReviaWindow::EntryKind::InvestigationChecking;
            work.speaker = window.DisplayName();
            work.body = "Checking the fixture build log";
            window.chatEntries.push_back(work);
            window.chatTurnInFlight = true;
            window.RenderChat();
            const std::size_t index = window.chatEntries.size() - 1;
            emit window.chatHistory->anchorClicked(QUrl(QStringLiteral("thought:%1").arg(index)));
            expect(window.chatEntries[index].expanded && window.chatHistory->toPlainText().contains("Checking the fixture build log"),
                "a checking row must open when clicked while its turn runs");
            window.chatTurnInFlight = false;
            window.chatEntries.clear();
            window.ResetConversationPresentation();
            window.RenderChat();
        }

        // Opening a thought at the bottom leaves the reader where they were: neither a relayout (the
        // composer growing) nor the next streamed line may pull the opened block off the top.
        {
            window.chatEntries.clear();
            window.ResetConversationPresentation();
            QString thought;
            for (int line = 0; line < 40; ++line)
                thought += QStringLiteral("Step %1 of the scroll fixture's reasoning, one full sentence.\n").arg(line);
            for (int turn = 0; turn < 14; ++turn)
            {
                window.AppendChat("You", QStringLiteral("Scroll fixture question %1").arg(turn), true);
                window.AppendChat(window.DisplayName(),
                    QStringLiteral("Scroll fixture answer %1, long enough to wrap across the measure at least once.").arg(turn), false,
                    turn == 13 ? thought : QString());
            }
            SettleLayouts();
            QScrollBar* bar = window.chatHistory->verticalScrollBar();
            bar->setValue(bar->maximum());
            SettleLayouts();
            const QString thoughtLink = QStringLiteral("thought:%1").arg(window.chatEntries.size() - 1);
            const int atBottom = bar->value();
            emit window.chatHistory->anchorClicked(QUrl(thoughtLink));
            SettleLayouts();
            expect(bar->maximum() > atBottom && bar->value() == atBottom, "opening a thought at the bottom must not move the reader");
            window.messageInput->setPlainText(QStringLiteral("one\ntwo\nthree\nfour\nfive"));
            SettleLayouts();
            expect(bar->value() == atBottom, "a growing composer must not pull an opened thought off the top");
            // The reading column keeps the composer's edges while the scroll bar shows: centred on the
            // transcript's full width, with the bar taken from the right margin.
            {
                const QTextFrameFormat format = window.chatHistory->document()->rootFrame()->frameFormat();
                const int full = window.chatHistory->width() - 2 * window.chatHistory->frameWidth();
                const int left = std::max(16, (full - 760) / 2);
                const int column = window.chatHistory->viewport()->width() - static_cast<int>(format.leftMargin()) -
                                   static_cast<int>(format.rightMargin());
                expect(bar->isVisible() && static_cast<int>(format.leftMargin()) == left && column == std::min(760, full - 2 * left),
                    "the transcript's column must keep its place and width when the scroll bar shows");
            }
            // One line pitch for replies and bubbles, 1.6 times the chat text size in pixels.
            expect(window.TranscriptCss().contains(QStringLiteral("line-height: %1px").arg(std::lround(window.chatTextSize * 1.6))),
                "the transcript's line pitch must be a pixel height that follows the chat text size");
            window.messageInput->clear();
            SettleLayouts();

            // The same while the turn is still running, and the next streamed line arrives.
            emit window.chatHistory->anchorClicked(QUrl(thoughtLink));
            SettleLayouts();
            bar->setValue(bar->maximum());
            SettleLayouts();
            window.chatTurnInFlight = true;
            const int streaming = bar->value();
            emit window.chatHistory->anchorClicked(QUrl(thoughtLink));
            SettleLayouts();
            window.AppendChat(window.DisplayName(), QStringLiteral("One more streamed sentence."));
            SettleLayouts();
            expect(bar->value() == streaming, "a streamed line must not pull a thought opened mid-turn off the top");
            window.chatTurnInFlight = false;
            window.chatEntries.clear();
            window.ResetConversationPresentation();
            window.RenderChat();
        }

        // A turn reads in order: the speaker line, the work, the thought process, the answer, what
        // was remembered, and last the actions that close it.
        {
            window.chatEntries.clear();
            window.ResetConversationPresentation();
            window.AppendChat("You", "Order fixture question", true);
            window.AppendWorkEntry(ReviaWindow::EntryKind::InvestigationChecking, "Reading the order fixture.", QString(), 0, 1);
            window.AppendChat(window.DisplayName(), "Order fixture answer.", false, "Order fixture reasoning.");
            RuntimeEvent memory;
            memory.kind = RuntimeEventKind::Memory;
            memory.message = "Remembered: the order fixture.";
            window.HandleRuntimeEvent(memory);
            window.RenderChat();
            const QString shown = window.chatHistory->toPlainText();
            const qsizetype speaker = shown.indexOf(QChar(0x25CF));
            const qsizetype work = shown.indexOf("is checking");
            const qsizetype thought = shown.indexOf("Thought process");
            const qsizetype answer = shown.indexOf("Order fixture answer.");
            const qsizetype remembered = shown.indexOf("Remembered: the order fixture.");
            const qsizetype copy = shown.lastIndexOf("Copy");
            expect(speaker >= 0 && speaker < work && work < thought && thought < answer && answer < remembered && remembered < copy &&
                       shown.count(QChar(0x25CF)) == 1,
                "a turn must read speaker, work, thought process, answer, memory, then Copy");
            window.chatEntries.clear();
            window.ResetConversationPresentation();
            window.RenderChat();
        }

        // A failed turn is one row after the turn, with Retry; no chat entry is added for it.
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.AppendChat("You", "Fixture question", true);
        const std::size_t entries = window.chatEntries.size();
        SessionResult failed;
        failed.stamp = window.Session().Stamp();
        failed.audienceRevision = window.Session().Audience().revision;
        failed.succeeded = false;
        failed.reason = "the fixture model was unavailable";
        window.PresentSessionResult(failed);
        window.RenderChat();
        QString html = window.chatHistory->toHtml();
        expect(html.contains("Couldn't finish") && html.contains("retry:") && window.chatEntries.size() == entries,
            "a failed turn must draw its own row with Retry and leave the transcript's entries alone");
        window.userStopRequested = true;
        window.PresentSessionResult(failed);
        window.RenderChat();
        html = window.chatHistory->toPlainText();
        expect(html.contains("Stopped") && !html.contains("Couldn't finish"), "a turn the owner stopped must read Stopped, without Retry");
        window.userStopRequested = false;
        window.lastTurnError.reset();

        // Starting is not a turn: Send keeps its slot, and the outlined Stop beside it cancels the startup.
        window.UpdateState(RuntimeState::Starting, "Loading models");
        expect(!window.sendButton->isHidden() && window.stopButton->isEnabled() && !window.stopButton->property("primary").toBool() &&
                   window.stopButton->accessibleName() == "Cancel startup",
            "while Revia starts, Send must stay in its slot beside a Cancel startup button");
        {
            // Esc while typing during the startup is a reflex, not a request to give the startup up.
            bool cancelled = false;
            const auto startupProbe = QObject::connect(window.stopButton, &QPushButton::clicked, &window, [&cancelled]() { cancelled = true; });
            window.editingLastTurn = false;
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(window.messageInput, &escape);
            QObject::disconnect(startupProbe);
            expect(!cancelled && !window.userStopRequested && !window.stopButton->toolTip().contains("Esc"),
                "Esc in the message box must not cancel the startup");
        }

        // Stop takes Send's slot while a turn runs, and Esc stops it.
        window.UpdateState(RuntimeState::Thinking, "Working on the fixture");
        expect(window.stopButton->isEnabled() && window.stopButton->property("primary").toBool() && window.sendButton->isHidden(),
            "while a turn runs, Stop must take Send's place");
        bool stopped = false;
        const auto probe = QObject::connect(window.stopButton, &QPushButton::clicked, &window, [&stopped]() { stopped = true; });
        window.editingLastTurn = false;
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(window.messageInput, &escape);
        QObject::disconnect(probe);
        expect(stopped && window.userStopRequested, "Esc in the message box must press Stop while it is enabled");
        window.userStopRequested = false;
        window.UpdateState(RuntimeState::Idle, "Ready");
        expect(!window.sendButton->isHidden(), "Send must come back once nothing runs");

        // Scrolled up, the transcript keeps its place and offers the latest message.
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        for (int line = 0; line < 25; ++line)
            window.AppendChat("System", QString("Pinning fixture line %1 with enough words to take a full row of the transcript.").arg(line));
        SettleLayouts();
        QScrollBar* bar = window.chatHistory->verticalScrollBar();
        expect(bar->maximum() > 100, "the pinning fixture must be long enough to scroll");
        bar->setValue(0);
        window.AppendChat("System", "Arrived while scrolled up.");
        SettleLayouts();
        expect(bar->value() == 0 && window.jumpToLatest->isVisible(), "a new message must not move a reader who scrolled up, and Latest must show");
        window.jumpToLatest->click();
        expect(bar->value() == bar->maximum() && !window.jumpToLatest->isVisible(), "Latest must return to the newest message");
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.RenderChat();

        // Desktop control shows itself in the header, with its own Stop, until the action ends. Stop
        // above latched desktop control off (as it does for the owner); release the latch first.
        window.Session().ResumeDesktopControl();
        RuntimeEvent automation;
        automation.kind = RuntimeEventKind::ComponentStatus;
        automation.component = "Automation";
        automation.phase = "Running";
        automation.message = "Executing a fixture action.";
        window.HandleRuntimeEvent(automation);
        window.UpdateState(RuntimeState::Acting, "Executing a fixture action.");
        auto* privacyDesktop = window.findChild<QToolButton*>("privacyDesktop");
        auto* headerStop = window.findChild<QPushButton*>("headerDesktopStop");
        expect(privacyDesktop && privacyDesktop->isVisible() && headerStop && headerStop->isVisible() &&
                   window.stateLabel->text() == "Controlling desktop",
            "an active desktop action must show its indicator, the header Stop and the Controlling desktop state");
        // With every indicator up and a long detail, the header still fits the narrowest window: the
        // detail is elided to the room left, never the reason the window cannot shrink.
        {
            const QSize before = window.size();
            window.perceptionWatching = true;
            window.screenAwarenessRunning = true;
            window.RefreshPrivacyIndicators();
            window.UpdateState(RuntimeState::Acting, "Executing a fixture action whose detail runs far longer than any header has room for");
            window.resize(760, 540);
            SettleLayouts();
            expect(window.width() <= 760 && window.chatHeader->minimumSizeHint().width() <= window.chatHeader->width() &&
                       (window.stateDetailLabel->isHidden() ||
                           window.stateDetailLabel->geometry().right() < window.findChild<QToolButton*>("chatMoreButton")->geometry().left()),
                "the chat header must fit a 760px window with every indicator and a long detail");
            window.perceptionWatching = false;
            window.screenAwarenessRunning = false;
            window.RefreshPrivacyIndicators();
            window.UpdateState(RuntimeState::Acting, "Executing a fixture action.");
            window.resize(before);
            SettleLayouts();
        }
        automation.phase = "Ready";
        window.HandleRuntimeEvent(automation);
        window.UpdateState(RuntimeState::Idle, "Ready");
        expect(privacyDesktop && !privacyDesktop->isVisible() && headerStop && !headerStop->isVisible(),
            "the desktop indicator and Stop must leave once the action ends");
        // The header Stop latches desktop control off, like the emergency stop it stands for.
        automation.phase = "Running";
        window.HandleRuntimeEvent(automation);
        if (headerStop)
            headerStop->click();
        expect(window.Session().DesktopControlStopped() && privacyDesktop && !privacyDesktop->isVisible(),
            "the header Stop must stop desktop control and take its indicator down");
        automation.phase = "Ready";
        window.HandleRuntimeEvent(automation);
        window.userStopRequested = false;
        SettleLayouts();

        // A fenced block is one framed box, not a strip per line.
        const QString code = revia::desktop::RenderUntrustedMarkdown("Run:\n\n```\ncmake --preset debug\ncmake --build build/debug\n```\n\nDone.");
        expect(code.count("<table") == 1 && code.contains(revia::desktop::theme::Current().codeBackground.name(), Qt::CaseInsensitive) &&
                   code.indexOf("cmake --preset") > code.indexOf("<table") && code.indexOf("cmake --build") < code.indexOf("</table>"),
            "a fenced code block must render as one box holding all its lines");
        // An empty chat's greeting sits well below the header, and the state pill is a real pill.
        window.chatEntries.clear();
        window.lastTurnError.reset();
        window.ResetConversationPresentation();
        window.RenderChat();
        SettleLayouts();
        expect(window.chatHistory->document()->rootFrame()->frameFormat().topMargin() > 24,
            "the empty chat's greeting must sit a third of the way down, not under the header");
        expect(window.statePill->height() == 28, "the state pill must be 28px tall so its ends draw round");
        // The sidebar menu keeps the panels in a submenu that names their groups, so the menu itself
        // stays short enough for a laptop screen.
        QStringList headings;
        for (QAction* action : window.sidebarPanelsMenu->actions())
        {
            if (auto* section = dynamic_cast<QWidgetAction*>(action);
                section != nullptr && section->defaultWidget() != nullptr && section->defaultWidget()->objectName() == "sidebarMenuSection")
                headings.push_back(section->text());
        }
        expect(headings == QStringList({"Workspace", "Companion", "Studio", "Runtime"}),
            "the sidebar menu must head its panel groups with Workspace, Companion, Studio and Runtime");
        expect(window.sidebarMenu->actions().size() <= 14 && window.sidebarMenu->sizeHint().height() < 520,
            "the sidebar menu must stay short enough to open above its button on a laptop screen");

        if (const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR"); !renderDirectory.isEmpty())
            expect(window.grab().save(QDir(renderDirectory).filePath("shell-1040x720.png")), "the shell must have a fresh Qt capture");
    }

    // The two number rows the window adds to Settings follow a change made elsewhere (what /set does)
    // and never write their old value back when focus only passes through them.
    static void RunSettingsReadBackUi(ReviaWindow& window, int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        const auto drainWrites = [&window]()
        {
            std::atomic<bool> drained = false;
            window.settingsWorker->Post([&drained]() { drained.store(true); });
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!drained.load() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            SettleLayouts();
        };
        window.RefreshPreferenceSpins();
        const int shown = window.initiativeMaxSpin->value();
        const int outside = shown == 12 ? 11 : 12;
        expect(window.Session().SetPreference("initiative.maxPerHour", std::to_string(outside)).succeeded,
            "the read-back fixture could not change the openings per hour");
        emit window.initiativeMaxSpin->editingFinished();
        drainWrites();
        expect(window.Session().UserPreferences().initiativeMaxPerHour == outside,
            "leaving an untouched number row wrote its old value over a change made elsewhere");
        window.OpenSettings("Chat");
        SettleLayouts();
        expect(window.initiativeMaxSpin->value() == outside, "opening Settings must show a number changed elsewhere");
        window.settingsDialog->hide();
        window.initiativeMaxSpin->setValue(outside - 1);
        emit window.initiativeMaxSpin->editingFinished();
        drainWrites();
        expect(window.Session().UserPreferences().initiativeMaxPerHour == outside - 1, "an edited number row must be saved");
        window.Session().SetPreference("initiative.maxPerHour", std::to_string(shown));
        window.RefreshPreferenceSpins();

        const std::string originalInitiativeMode = window.Session().UserPreferences().initiativeMode;
        expect(window.Session().SetPreference("initiative.mode", "focus").succeeded,
            "the read-back fixture could not change conversation mode");
        window.RefreshSettingsExtras();
        expect(window.initiativeModeCombo->currentData().toString() == QStringLiteral("focus"),
            "the conversation mode did not follow a preference changed elsewhere");
        const int talkative = window.initiativeModeCombo->findData(QStringLiteral("talkative"));
        window.initiativeModeCombo->setCurrentIndex(talkative);
        emit window.initiativeModeCombo->activated(talkative);
        drainWrites();
        expect(window.Session().UserPreferences().initiativeMode == "talkative",
            "the conversation mode was not saved by the Settings worker");
        window.Session().SetPreference("initiative.mode", originalInitiativeMode);
        window.RefreshInitiativeMode();

        const std::string originalDelegation = window.Session().UserPreferences().delegation;
        const std::string outsideDelegation = originalDelegation == "off" ? "on_request" : "off";
        expect(window.Session().SetPreference("conversation.delegation", outsideDelegation).succeeded,
            "the read-back fixture could not change delegation mode");
        window.RefreshSettingsExtras();
        expect(window.delegationModeCombo->currentData().toString().toStdString() == outsideDelegation,
            "the delegation choice did not follow a preference changed elsewhere");
        const int autoDelegation = window.delegationModeCombo->findData(QStringLiteral("auto"));
        window.delegationModeCombo->setCurrentIndex(autoDelegation);
        emit window.delegationModeCombo->activated(autoDelegation);
        drainWrites();
        expect(window.Session().UserPreferences().delegation == "auto", "the delegation choice was not saved by the Settings worker");
        window.Session().SetPreference("conversation.delegation", originalDelegation);
        window.RefreshDelegationMode();

        // The window's own controls in Settings take the wheel only once focused, and choosing Hard
        // questions, which sends turns to the cloud without asking each time, asks first.
        const auto settingsPath = window.Session().Paths().InstallRoot() / "Config/settings.json";
        const bool createSettings = !std::filesystem::exists(settingsPath);
        struct RestoreSettingsFile
        {
            std::filesystem::path path;
            bool created;
            ~RestoreSettingsFile()
            {
                std::error_code error;
                if (created)
                    std::filesystem::remove(path, error);
            }
        } restoreSettings{settingsPath, createSettings};
        if (createSettings)
            std::ofstream(settingsPath) << nlohmann::json{{"llm", {{"backend", "None"}, {"autoStartServer", false}}},
                {"intelligence", {{"enabled", false}}}, {"embedding", {{"enabled", false}}},
                {"speechRecognition",
                    {{"enabled", false}}}}.dump();
        window.OpenSettings("Models");
        SettleLayouts();
        expect(window.cloudUseForCombo->focusPolicy() == Qt::StrongFocus && window.initiativeMaxSpin->focusPolicy() == Qt::StrongFocus &&
                   window.resourceSampleSpin->focusPolicy() == Qt::StrongFocus,
            "the window's Settings controls must not change under a passing wheel");
        const QString savedUse = window.cloudUseForReader ? window.cloudUseForReader("llm.cloud.useFor") : QString();
        expect(window.imageMinTokensCombo && window.imageMinTokensCombo->count() == 6 &&
                   window.imageMinTokensCombo->itemData(0).toString() == "0" &&
                   window.imageMinTokensCombo->itemText(0).contains("backend default", Qt::CaseInsensitive) &&
                   window.imageMinTokensCombo->itemData(5).toString() == "4096" &&
                   window.imageMinTokensCombo->toolTip().contains("restart", Qt::CaseInsensitive),
            "image detail does not expose its bounded choices, backend default and restart requirement");
        const std::string savedImageDetail = window.Session().OwnerChoice("llm.imageMinTokens");
        const int detail = window.imageMinTokensCombo->findData(QStringLiteral("1024"));
        window.imageMinTokensCombo->setCurrentIndex(detail);
        emit window.imageMinTokensCombo->activated(detail);
        drainWrites();
        expect(
            window.Session().OwnerChoice("llm.imageMinTokens") == "1024" && window.imageMinTokensCombo->currentData().toString() == "1024",
            "image detail choice was not persisted and read back through the owner settings worker");
        window.Session().SetOwnerChoice("llm.imageMinTokens", savedImageDetail.empty() ? "0" : savedImageDetail);
        window.RefreshImageMinTokens();
        const int hard = window.cloudUseForCombo->findData(QStringLiteral("hardTurns"));
        window.cloudUseForCombo->setCurrentIndex(hard);
        emit window.cloudUseForCombo->activated(hard);
        SettleLayouts();
        auto* box = window.settingsDialog->findChild<QMessageBox*>("settingConfirmDialog");
        expect(box && box->isVisible() && box->defaultButton() == box->button(QMessageBox::Cancel) &&
                   window.cloudUseForCombo->currentIndex() != hard,
            "choosing Hard questions must ask first and keep the saved choice shown meanwhile");
        if (box)
            box->button(QMessageBox::Cancel)->click();
        SettleLayouts();
        expect((window.cloudUseForReader ? window.cloudUseForReader("llm.cloud.useFor") : QString()) == savedUse,
            "Cancel must keep the saved cloud choice");
        window.settingsDialog->hide();
    }

    // Quit from a visible window: the shutdown's QApplication::quit() closes every window first and
    // gives up if one refuses, so the main window must accept its close once shutdown has begun.
    static void RunShutdownUi(int& failures)
    {
        ReviaWindow window(false, false);
        window.resize(1040, 720);
        window.show();
        SettleLayouts();
        bool watchdogFired = false;
        QTimer::singleShot(0, &window, [&window]() { window.BeginShutdown(revia::core::ExitReason::UserCommand, "shutdown fixture"); });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, [&watchdogFired]()
        {
            watchdogFired = true;
            QCoreApplication::exit(42);
        });
        watchdog.start(10000);
        const int code = QCoreApplication::exec();
        watchdog.stop();
        if (watchdogFired || code != 0 || window.isVisible())
        {
            std::cerr << "FAIL: quitting with the window visible must end the event loop (exec returned " << code << ")\n";
            ++failures;
        }
    }

    static void RunMemoryCorrectionProposalUi(ReviaWindow& window, int& failures)
    {
        using revia::runtime::ReviaSessionTestAccess;
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
        };
        const auto wait = [&](const auto& done)
        {
            QElapsedTimer timer;
            timer.start();
            while (!done() && timer.elapsed() < 3000)
            {
                QCoreApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return done();
        };
        std::string error;
        const auto previousAudience = window.Session().Audience();
        const bool wasStarted = window.Session().IsStarted();
        ReviaSessionTestAccess::MarkStudioStarted(window.Session(), true);
        struct RestoreStarted
        {
            revia::runtime::ReviaSession& session;
            bool started;
            ~RestoreStarted() { ReviaSessionTestAccess::MarkStudioStarted(session, started); }
        } restoreStarted{window.Session(), wasStarted};
        expect(window.Session().SetAudience({revia::identity::AudienceKind::Private, "proposal-ui", 0, {}}, error),
            "proposal UI requires private audience fixture");
        ReviaSessionTestAccess::DisableQualityFixtureEmbedding(window.Session());
        expect(!ReviaSessionTestAccess::IntroduceTaskSpeaker(window.Session(), "ProposalUiOwner").empty(),
            "proposal UI requires explicit participant");
        (void)ReviaSessionTestAccess::StudioInput(window.Session(), revia::agents::InputSource::Typed);
        const auto scope = ReviaSessionTestAccess::CorrectionScope(window.Session());
        longTermMemory store(window.Session().Paths().Resolve("Memory/revia_memory.db").string());
        const auto makeProposal = [&](const std::string& suffix)
        {
            memoryDecision old;
            old.bSuccess = old.bShouldRemember = true;
            old.category = "preference";
            old.summary = "The user prefers tea in the " + suffix + " fixture.";
            old.subject = revia::memory::ParticipantSubject(scope);
            auto corrected = old;
            corrected.summary = "The user prefers coffee in the " + suffix + " fixture.";
            bool added = false;
            std::string oldId, newId;
            expect(store.Save(old, added, &oldId) && store.Save(corrected, added, &newId), "proposal UI saved pair fixture");
            memoryEntry original;
            for (const auto& row : store.Load()) if (row.id == oldId) original = row;
            return revia::memory::MemoryCorrectionProposal{"ui-proposal-" + suffix, original,
                revia::audit::ContentDigest(original.summary), newId, corrected, "Actually I now prefer coffee.", scope, 17};
        };
        const auto rejected = makeProposal("rejected");
        const auto accepted = makeProposal("accepted");
        const auto beforeCount = store.Load().size();
        const auto beforeReceipts = store.RevisionHistory().size();
        ReviaSessionTestAccess::QueueMemoryCorrections(window.Session(), {rejected, accepted});
        window.ConfigureMemoryRevisionControls();
        window.memoryPanel->Refresh();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* selector = window.findChild<QComboBox*>("memoryCorrectionSelector");
        auto* review = window.findChild<QPushButton*>("memoryCorrectionReview");
        auto* reject = window.findChild<QPushButton*>("memoryCorrectionReject");
        expect(selector && review && reject && selector->count() == 2, "pending exact correction selector must show both proposals");
        if (!selector || !review || !reject) return;
        selector->setCurrentIndex(selector->findData(QString::fromStdString(rejected.id)));
        reject->click();
        expect(wait([&] { return !window.studioOperationRunning.load(); }), "proposal rejection must complete through studio owner");
        expect(store.Load().size() == beforeCount && store.RevisionHistory().size() == beforeReceipts && selector->count() == 1,
            "reject must remove only the pending proposal, with both saved facts intact");
        selector->setCurrentIndex(selector->findData(QString::fromStdString(accepted.id)));
        review->click();
        auto* original = window.findChild<QPlainTextEdit*>("memoryRevisionOriginal");
        auto* corrected = window.findChild<QPlainTextEdit*>("memoryRevisionCorrected");
        auto* record = window.findChild<QPushButton*>("memoryRevisionRecord");
        expect(original && corrected && record && original->isReadOnly() && corrected->isReadOnly() &&
            original->toPlainText().toStdString() == accepted.original.summary &&
            corrected->toPlainText().toStdString() == accepted.corrected.summary,
            "review must display the selected exact old and new saved text read-only");
        expect(store.RevisionHistory().size() == beforeReceipts, "opening review must not grant owner revision authority");
        if (corrected && record)
        {
            const auto renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
            if (!renderDirectory.isEmpty())
            {
                SettleLayouts();
                expect(corrected->window()->grab().save(QDir(renderDirectory).filePath("memory-correction-proposal.png")),
                    "proposal review must have a fresh Qt capture");
            }
            // Even a programmatic widget change cannot change the pair submitted:
            // the UI sends only its selected proposal ID to the session owner.
            corrected->setPlainText("UI tamper sentinel must never become a saved fact");
            record->click();
            expect(wait([&] { return !window.studioOperationRunning.load(); }), "proposal acceptance must complete through studio owner");
            const auto receipts = store.RevisionHistory();
            expect(receipts.size() == beforeReceipts + 1 && receipts.back().originalId == accepted.original.id &&
                receipts.back().revisedId == accepted.proposedMemoryId && !receipts.back().wasAdded &&
                store.Load().size() == beforeCount && selector->count() == 0,
                "accept must bind the selected pair through one receipt and reuse the exact saved replacement");
        }
        const auto expired = makeProposal("scope-expired");
        ReviaSessionTestAccess::QueueMemoryCorrections(window.Session(), {expired});
        window.memoryPanel->Refresh();
        review->click();
        expect(window.Session().SetAudience({revia::identity::AudienceKind::Public, "proposal-public", 0, {}}, error),
            "proposal scope-expiry fixture");
        expect(wait([&] { return selector->count() == 0; }), "scope change must remove private pending proposals from the panel");
        expect(!window.findChild<QPlainTextEdit*>("memoryRevisionCorrected"), "scope expiry must close the private proposal snapshot");
        expect(window.Session().SetAudience(previousAudience, error), "restore previous fixture audience");
    }

    static void RunConfirmationAudienceUi(ReviaWindow& window, int& failures)
    {
        using namespace revia::actions;
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        struct PromptProbe final : QObject
        {
            ReviaWindow* owner = nullptr;
            QString title;
            std::function<void()> beforeDelivery;
            std::function<void(QMessageBox&)> onShow;
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (watched == owner && event->type() == QEvent::MetaCall && beforeDelivery)
                {
                    auto transition = std::move(beforeDelivery);
                    beforeDelivery = {};
                    transition();
                }
                if (event->type() == QEvent::Show)
                {
                    if (auto* prompt = qobject_cast<QMessageBox*>(watched); prompt && prompt->windowTitle() == title && onShow)
                        onShow(*prompt);
                }
                return false;
            }
        } probe;
        probe.owner = &window;
        QApplication::instance()->installEventFilter(&probe);
        ActionRequest request;
        request.type = ActionType::ReadTextFile;
        request.source = window.Session().Paths().InstallRoot() / "private-confirmation-source-sentinel";
        const PolicyDecision decision{PolicyVerdict::RequiresConfirmation, RiskLevel::ReadOnly, "Synthetic private prompt fixture."};
        const revia::policy::ApprovalPrompt effect{"Synthetic fixture control", "synthetic-fixture-application",
            "private-consequence-window-sentinel", "private-consequence-reason-sentinel"};
        std::string audienceError;
        const auto makePrivate = [&]()
        {
            expect(window.Session().SetAudience({revia::identity::AudienceKind::Private, "approval-private", 0, {}}, audienceError),
                "the approval fixture must select an explicit private audience");
            QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        };
        const auto changeAudience = [&](const bool returnToPrivate)
        {
            expect(window.Session().SetAudience({revia::identity::AudienceKind::Public, "approval-public", 0, {}}, audienceError),
                "a synthetic approval must cross its captured private audience");
            if (returnToPrivate)
                expect(window.Session().SetAudience({revia::identity::AudienceKind::Private, "approval-private", 0, {}}, audienceError),
                    "a synthetic approval must retain its original revision after public-to-private");
        };
        for (const bool consequence : {false, true})
        {
            probe.title = consequence ? "Approve this action" : "Confirm Revia action";
            const auto ask = [&]()
            {
                return consequence ? window.ApproveDesktopEffect(effect)
                                   : window.ConfirmAction(request, decision) == ConfirmationChoice::Allow;
            };
            const auto clickConsent = [&](QMessageBox& prompt)
            {
                for (auto* button : prompt.findChildren<QPushButton*>())
                {
                    if ((consequence && prompt.standardButton(button) == QMessageBox::Yes) ||
                        (!consequence && button->text() == "Allow once"))
                    {
                        button->click();
                        return true;
                    }
                }
                prompt.reject();
                return false;
            };
            for (const bool returnToPrivate : {false, true})
            {
                makePrivate();
                int promptsShown = 0;
                probe.beforeDelivery = [&]() { changeAudience(returnToPrivate); };
                probe.onShow = [&](QMessageBox& prompt)
                {
                    ++promptsShown;
                    QTimer::singleShot(0, &prompt, [&prompt]() { prompt.reject(); });
                };
                std::atomic<bool> finished = false;
                bool choice = true;
                std::jthread worker(
                    [&]()
                    {
                        choice = ask();
                        finished.store(true);
                    });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!finished.load() && std::chrono::steady_clock::now() < deadline)
                {
                    QCoreApplication::processEvents();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (!finished.load())
                    window.AbandonQuestions();
                worker.join();
                expect(!choice && promptsShown == 0 && !probe.beforeDelivery,
                    consequence
                        ? "a queued private consequence approval must not display its application/window/reason after an audience epoch"
                        : "a queued private confirmation must not display its source paths after an audience epoch");
                probe.beforeDelivery = {};
                if (window.questions.IsAbandoned())
                    window.questions.Reopen();
            }
            makePrivate();
            bool currentPromptShown = false;
            probe.onShow = [&](QMessageBox& prompt)
            {
                const bool descriptionMatches = consequence ? prompt.text().contains("synthetic-fixture-application") &&
                                                                  prompt.text().contains("private-consequence-window-sentinel") &&
                                                                  prompt.text().contains("private-consequence-reason-sentinel")
                                                            : prompt.text().contains("private-confirmation-source-sentinel");
                currentPromptShown = descriptionMatches && prompt.defaultButton() &&
                                     (consequence ? prompt.standardButton(prompt.defaultButton()) == QMessageBox::No
                                                  : prompt.defaultButton()->text() == "No");
                QTimer::singleShot(0, &prompt, [&prompt]() { prompt.reject(); });
            };
            expect(!ask() && currentPromptShown,
                consequence ? "a current private consequence approval must display its exact context with default No"
                            : "a current private confirmation must display its exact synthetic source with default No");
            makePrivate();
            bool currentConsentClicked = false;
            probe.onShow = [&](QMessageBox& prompt)
            { QTimer::singleShot(0, &prompt, [&]() { currentConsentClicked = clickConsent(prompt); }); };
            expect(ask() && currentConsentClicked, consequence ? "a current private synthetic Yes must preserve consequence approval"
                                                               : "a current private synthetic Allow once must preserve typed confirmation");
            makePrivate();
            bool activePromptShown = false;
            bool fallbackClosed = false;
            QTimer fallback;
            fallback.setSingleShot(true);
            QObject::connect(&fallback, &QTimer::timeout, &window,
                [&]()
                {
                    fallbackClosed = true;
                    if (window.openQuestion)
                        window.openQuestion->reject();
                });
            probe.onShow = [&](QMessageBox& prompt)
            {
                activePromptShown = true;
                QTimer::singleShot(0, &prompt, [&]() { changeAudience(true); });
            };
            fallback.start(1000);
            expect(!ask() && activePromptShown && !fallbackClosed,
                consequence ? "an active private consequence approval must close after its audience retires without owner input"
                            : "an active private confirmation must close after its audience retires without owner input");
            fallback.stop();
            makePrivate();
            bool staleConsentClicked = false;
            probe.onShow = [&](QMessageBox& prompt)
            {
                QTimer::singleShot(0, &prompt,
                    [&]()
                    {
                        changeAudience(true);
                        staleConsentClicked = clickConsent(prompt);
                    });
            };
            expect(!ask() && staleConsentClicked,
                consequence ? "a synthetic Yes after an audience epoch must not authorize the retired consequence approval"
                            : "a synthetic Allow once after an audience epoch must not authorize the retired confirmation");
            probe.onShow = {};
        }
    }

    static void RunChatTranscriptUi(ReviaWindow& window, int& failures)
    {
        using namespace revia::runtime;
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        // A second Enter while the foreground worker still runs must return at once. It
        // used to join that worker on this thread and freeze the window until it ended.
        if (window.operationWorker.joinable())
            window.operationWorker.join();
        std::atomic<bool> release = false;
        window.operationRunning.store(true);
        window.operationWorker = std::jthread(
            [&]()
            {
                QElapsedTimer held;
                held.start();
                while (!release.load() && held.elapsed() < 4000)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                window.operationRunning.store(false);
            });
        window.messageInput->setPlainText("second enter sentinel");
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QElapsedTimer pressed;
        pressed.start();
        QCoreApplication::sendEvent(window.messageInput, &enter);
        expect(pressed.elapsed() < 1500 && window.messageInput->toPlainText() == "second enter sentinel",
            "Enter during a running operation waited on the worker or discarded the typed text");
        release.store(true);
        window.operationWorker.join();
        window.messageInput->clear();

        // Replies render as Markdown, and nothing in reply text is interpreted as markup
        // or becomes a working link. A user line first, so Copy covers this reply only.
        window.AppendChat("You", "Show me the build command.", true);
        const auto before = window.chatEntries.size();
        const QString reply = "Use `make` here <b>raw</b><script>alert(1)</script> [regen](copy:0) ![x](file:///C:/secret.png)\n\n"
                              "```\nint value = 1;\n```";
        RuntimeEvent answer{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, reply.toStdString()};
        answer.audienceRevision = window.Session().Audience().revision;
        window.HandleRuntimeEvent(answer);
        expect(window.chatEntries.size() == before + 1 && window.chatEntries.back().markdown, "the reply was not kept as Markdown");
        const QString shown = window.chatHistory->toHtml();
        expect(!shown.contains("<script") && shown.contains("&lt;script&gt;") && !shown.contains("<b>raw"),
            "reply HTML was interpreted instead of shown as text");
        expect(!shown.contains("file:///C:/secret.png\"") && shown.contains("[image: file:///C:/secret.png]"),
            "a reply image was loaded instead of shown as its address");
        const QString copyLink = QString("copy:%1").arg(window.chatEntries.size() - 1);
        expect(shown.count("href=\"copy:") >= 1 && shown.contains("href=\"" + copyLink + "\"") && !shown.contains("href=\"copy:0\""),
            "the reply has no Copy link of its own, or its text added one");
        expect(shown.contains("Consolas") && shown.contains("int value = 1;"), "code was not set in a monospace face");
        emit window.chatHistory->anchorClicked(QUrl(copyLink));
        expect(QApplication::clipboard()->text() == reply, "Copy did not place the reply's own text on the clipboard");

        // The transcript is capped, and streamed fragments still replace the right entry.
        RuntimeEvent fragment{RuntimeEventKind::ReplyFragment, RuntimeState::Responding, "Streamed first sentence."};
        fragment.audienceRevision = window.Session().Audience().revision;
        fragment.conversationTurnId = 7707;
        window.HandleRuntimeEvent(fragment);
        for (int index = 0; index < 450; ++index)
            window.AppendChat("System", QString("filler %1").arg(index));
        expect(window.chatEntries.size() == ReviaWindow::MaximumChatEntries, "the transcript grew past its cap");
        RuntimeEvent replace = fragment;
        replace.phase = "Replace";
        replace.message = "Approved whole reply after trimming.";
        window.HandleRuntimeEvent(replace);
        expect(window.chatEntries.back().body == "Approved whole reply after trimming.",
            "a replacement for a fragment that scrolled away was lost");
        expect(std::count_if(window.chatEntries.begin(), window.chatEntries.end(),
                   [](const auto& entry) { return entry.body.startsWith("filler"); }) == static_cast<long>(ReviaWindow::MaximumChatEntries) - 1,
            "trimming removed the wrong entries");

        // Streamed thinking for one turn updates one working entry instead of adding one
        // per delta; a different turn still gets its own.
        window.chatEntries.clear();
        const auto thinking = [&](std::uint64_t turn, const char* phase, const char* text)
        {
            RuntimeEvent event{RuntimeEventKind::SelfInquiry, RuntimeState::Thinking, text};
            event.turnId = turn;
            event.phase = phase;
            event.detail = "fixture inquiry";
            window.HandleRuntimeEvent(event);
        };
        thinking(9101, "Thinking", "First I check");
        thinking(9101, "Thinking", "First I check the inputs.");
        thinking(9101, "Thinking", " Then the units.");
        thinking(9101, "Asking", "Q1: is the unit metric?");
        thinking(9102, "Thinking", "A separate turn.");
        const auto inquiries = std::count_if(window.chatEntries.begin(), window.chatEntries.end(),
            [](const auto& entry) { return entry.kind == ReviaWindow::EntryKind::SelfInquiry; });
        expect(inquiries == 2 && window.chatEntries.front().body == "First I check the inputs. Then the units.\n\nQ1: is the unit metric?" &&
                   window.chatEntries.back().body == "A separate turn.",
            "streamed self-inquiry for one turn did not update a single entry in place");
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.RenderChat();
    }

    static void RunChatSessionsUi(ReviaWindow& window, int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        auto* panel = dynamic_cast<ChatSessionsPanel*>(window.findChild<QWidget*>("chatSessionsPanel"));
        auto* list = window.findChild<QListWidget*>("chatSessionList");
        auto* status = window.findChild<QLabel*>("chatSessionsStatus");
        expect(panel && list && status && window.findChild<QPushButton*>("chatSidebarToggle"), "Chat has no chat list beside the transcript");
        if (!panel || !list || !status)
            return;
        panel->Refresh();
        expect(list->count() == 0 && status->text().contains("once Revia has started"), "the chat list did not explain why it is empty");
        // Beside the transcript, the list must not make the chat page taller than the
        // window: that scrolled the message box out of view.
        revia::desktop::SelectNavigationPage(window.findChild<QWidget*>("chatPage"));
        window.resize(1040, 720);
        window.show();
        SettleLayouts();
        auto* chatScroll = window.findChild<QScrollArea*>("conversationPageScroll");
        expect(panel->isVisible() && chatScroll && chatScroll->verticalScrollBar()->maximum() == 0,
            "the chat list pushed the message box below the window");

        // With a started session and an archive holding one earlier chat.
        QTemporaryDir archiveDirectory;
        const auto path = std::filesystem::path(archiveDirectory.path().toStdWString()) / "conversations.db";
        {
            revia::memory::ConversationArchive seed(path.string());
            std::string error;
            seed.BeginSession("earlier-chat", error);
            seed.Record("earlier-chat", "user", "Plan the sentinel_garden_7a beds.", error);
            seed.Record("earlier-chat", "assistant", "Start with **sunny** corners.", error);
        }
        revia::runtime::ReviaSessionTestAccess::OpenConversationArchive(window.session, path, "current-chat");
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.session, true);
        window.chatEntries.clear();
        window.AppendChat("You", "Live question", true);
        panel->Refresh();
        QListWidgetItem* earlier = nullptr;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toString() == "earlier-chat")
                earlier = list->item(row);
        expect(list->count() == 2 && earlier && earlier->text().contains("sentinel_garden_7a"),
            "the earlier chat and the current one were not both listed");
        if (earlier)
        {
            // The open chat is highlighted after a refresh; choosing another row moves it, as a click does.
            QListWidgetItem* open = list->currentItem();
            expect(open != nullptr && open->isSelected() && open->data(Qt::UserRole).toString() == "current-chat",
                "the chat list did not highlight the open chat");
            list->setCurrentItem(earlier);
            QTemporaryDir exportDirectory;
            QString error;
            const QString markdownPath = exportDirectory.filePath("chat.md");
            const QString jsonPath = exportDirectory.filePath("chat.json");
            QFile markdown(markdownPath);
            QFile json(jsonPath);
            expect(panel->ExportSelected(markdownPath, false, error) && markdown.open(QIODevice::ReadOnly) &&
                       QString::fromUtf8(markdown.readAll()).contains("**You:**\n\nPlan the sentinel_garden_7a beds."),
                "Markdown export did not write the chat to the chosen file");
            expect(panel->ExportSelected(jsonPath, true, error) && json.open(QIODevice::ReadOnly) &&
                       QString::fromUtf8(json.readAll()).contains("\"role\": \"assistant\""),
                "JSON export did not write the chat to the chosen file");

            // Opening it shows it read-only; live entries keep collecting underneath.
            window.OpenArchivedChat("earlier-chat");
            const QString shown = window.chatHistory->toPlainText();
            expect(!window.viewingArchivedChat.empty() && window.archivedChatBanner->isVisibleTo(&window) &&
                       shown.contains("sentinel_garden_7a") && !shown.contains("Live question"),
                "opening a saved chat did not show it in place of the live transcript");
            window.AppendChat("System", "Arrived while reading", false);
            expect(!window.chatHistory->toPlainText().contains("Arrived while reading"), "a live entry replaced the saved chat on screen");
            window.CloseArchivedChat();
            expect(window.chatHistory->toPlainText().contains("Arrived while reading") && !window.archivedChatBanner->isVisibleTo(&window),
                "Back did not return to the live transcript");
            window.OpenArchivedChat("earlier-chat");
            window.ContinueArchivedChat();
            expect(!window.viewingArchivedChat.empty() && !window.archivedChatLabel->text().isEmpty() &&
                       window.Session().CurrentConversationId() == "current-chat",
                "an unidentified typist continued a chat, or the refusal was not shown");
            window.CloseArchivedChat();

            // Delete in the list acts on the highlighted row, never on the chat in progress behind it.
            panel->Refresh();
            for (int row = 0; row < list->count(); ++row)
                if (list->item(row)->data(Qt::UserRole).toString() == "earlier-chat")
                    list->setCurrentItem(list->item(row));
            list->setFocus(Qt::OtherFocusReason);
            QString asked;
            QTimer answer;
            answer.setInterval(20);
            QObject::connect(&answer, &QTimer::timeout, &window,
                [&asked, &answer]()
                {
                    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                    {
                        asked = box->text();
                        answer.stop();
                        box->button(QMessageBox::Yes)->click();
                    }
                });
            answer.start();
            const std::size_t liveEntries = window.chatEntries.size();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(list, &press);
            QApplication::sendEvent(list, &release);
            answer.stop();
            panel->Refresh();
            bool earlierListed = false;
            bool currentListed = false;
            for (int row = 0; row < list->count(); ++row)
            {
                earlierListed = earlierListed || list->item(row)->data(Qt::UserRole).toString() == "earlier-chat";
                currentListed = currentListed || list->item(row)->data(Qt::UserRole).toString() == "current-chat";
            }
            expect(!asked.contains("in progress") && !earlierListed && currentListed && window.chatEntries.size() == liveEntries &&
                       window.Session().CurrentConversationId() == "current-chat",
                "Delete in the chat list must delete the highlighted chat and leave the chat in progress alone");
        }
        std::string failure;
        expect(window.StartNewChat(failure) && window.chatEntries.empty() && window.Session().CurrentConversationId() != "current-chat",
            "New chat did not start a fresh transcript and archive session");
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.session, false);
        window.chatEntries.clear();
        window.RenderChat();
    }

    static void RunAttachmentUi(ReviaWindow& window, int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        const auto settle = [&]()
        {
            if (window.attachmentWorker.joinable())
                window.attachmentWorker.join();
            if (window.operationWorker.joinable())
                window.operationWorker.join();
            QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        const auto storedFiles = [&]()
        {
            const auto directory = window.Session().Paths().Resolve("RuntimeData/Attachments/Files");
            std::error_code error;
            std::size_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error))
                count += entry.is_regular_file() ? 1 : 0;
            return count;
        };
        auto* tray = dynamic_cast<AttachmentTray*>(window.findChild<QWidget*>("attachmentTray"));
        auto* attach = window.findChild<QPushButton*>("attachButton");
        auto* documents = window.findChild<QPushButton*>("documentsButton");
        expect(tray && attach && documents, "The composer has no attachment tray, Attach or Documents button");
        if (!tray || !attach || !documents)
            return;
        std::string audienceError;
        window.Session().SetAudience({revia::identity::AudienceKind::Private, "attach-private", 0, {}}, audienceError);
        window.chatEntries.clear();
        window.RenderChat();

        QTemporaryDir files;
        const QString notes = files.filePath("notes.txt");
        {
            QFile file(notes);
            file.open(QIODevice::WriteOnly);
            file.write("The garage code is sentinel_ui_attach_8c.\n");
        }
        const QString photo = files.filePath("photo.jpg");
        QImage big(2400, 1200, QImage::Format_RGB32);
        big.fill(Qt::darkCyan);
        big.save(photo, "JPG");

        // The Attach button's path: files are read on the worker and appear as chips.
        window.AttachPaths({notes, photo});
        settle();
        expect(tray->Chips().size() == 2 && tray->Chips()[0].name == "notes.txt" && !tray->Chips()[1].thumbnail.isNull(),
            "attached files did not appear as chips with a picture thumbnail");
        expect(window.findChildren<QFrame*>("attachmentChip").size() == 2 && storedFiles() == 2, "the chips or the private copies are missing");

        // Removing a chip deletes its private copy.
        const auto chips = window.findChildren<QToolButton*>("attachmentChipRemove");
        if (!chips.isEmpty())
            chips.front()->click();
        settle();
        expect(tray->Chips().size() == 1 && storedFiles() == 1, "removing a chip left it or its copy behind");

        // A picture pasted from the clipboard, and a file dropped on the message box.
        QImage pasted(64, 32, QImage::Format_RGB32);
        pasted.fill(Qt::magenta);
        QApplication::clipboard()->setImage(pasted);
        QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
        QCoreApplication::sendEvent(window.messageInput, &paste);
        settle();
        expect(tray->Chips().size() == 2 && tray->Chips().back().name.startsWith("pasted-image-"), "a pasted picture was not attached");
        QMimeData dropped;
        dropped.setUrls({QUrl::fromLocalFile(notes)});
        QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
        // As a real drag does: the drop target first accepts the drag, then receives the drop.
        QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window.messageInput->viewport(), &enter);
        QCoreApplication::sendEvent(window.messageInput->viewport(), &drop);
        settle();
        expect(tray->Chips().size() == 3 && window.messageInput->toPlainText().isEmpty(),
            "a dropped file was not attached, or its path was pasted as text");

        // Sending shows the files under the message. The session is not started, so the
        // turn is refused, and a message that was never answered keeps no files.
        window.messageInput->setPlainText("What do these say?");
        window.SendMessage();
        settle();
        const QString shown = window.chatHistory->toPlainText();
        expect(tray->IsEmpty() && shown.contains("Attached:") && shown.contains("notes.txt") && shown.contains("pasted-image-"),
            "the sent message does not list its attachments");
        expect(storedFiles() == 0, "files of a message that was never answered were kept");

        // A shared audience cannot attach.
        window.Session().SetAudience({revia::identity::AudienceKind::Shared, "attach-room", 0, {"viewer"}}, audienceError);
        window.AttachPaths({notes});
        settle();
        expect(tray->IsEmpty() && window.findChild<QLabel*>("attachmentStatus")->text().contains("private"),
            "a shared audience could attach a file, or was not told why not");
        window.Session().SetAudience({revia::identity::AudienceKind::Private, "attach-private", 0, {}}, audienceError);
        tray->SetStatus(QString());

        // The Documents list: view, then forget, a document kept from an earlier message.
        auto& library = revia::runtime::ReviaSessionTestAccess::Attachments(window.session);
        const auto kept = library.StageBytes("plan.md", "Plan: sentinel_ui_document_3e.\n");
        (void)library.PrepareTurn("plan", library.TakeStaged({kept.attachment.id}), {});
        window.ShowDocuments();
        DocumentsDialog* dialog = window.documentsDialog;
        expect(dialog && dialog->RowCount() == 1, "the Documents list did not show the kept document");
        if (dialog)
        {
            dialog->Select(kept.attachment.id);
            dialog->ViewSelected();
            expect(dialog->PreviewText().contains("sentinel_ui_document_3e"), "View did not show the document's text");
            QString error;
            expect(dialog->ForgetDocument(kept.attachment.id, error) && dialog->RowCount() == 0 && storedFiles() == 0 &&
                       window.Session().Documents().empty(),
                "Forget left the document, its copy or its list entry");
            dialog->close();
        }
        settle();
        window.chatEntries.clear();
        window.RenderChat();
    }

    static void RunAttachmentLifecycleUi(ReviaWindow& window, int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        const auto wait = [&](const std::function<bool()>& ready)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!ready() && std::chrono::steady_clock::now() < deadline)
            {
                QCoreApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            expect(ready(), "attachment lifecycle operation did not finish in its bounded window");
        };
        auto* tray = window.attachmentTray;
        tray->Clear();
        tray->SetStatus("The picture could not be decoded.");
        expect(!tray->isHidden(), "a first-attachment refusal is hidden with its empty tray");
        tray->SetStatus(QString());
        expect(tray->isHidden(), "clearing the last attachment status left an empty tray visible");
        QTemporaryDir animatedFiles;
        const QString animated = animatedFiles.filePath("motion.gif");
        {
            QFile gif(animated);
            expect(gif.open(QIODevice::WriteOnly), "the animation fixture could not be written");
            gif.write(QByteArray::fromHex("47494638396101000100800000000000ffffff"
                                          "21f90400010000002c0000000001000100000202440100"
                                          "21f90400010000002c00000000010001000002024401003b"));
        }
        QImageReader animation(animated);
        expect(animation.imageCount() == 2, "the GIF fixture does not contain two decodable frames");
        window.AttachPaths({animated});
        if (window.attachmentWorker.joinable())
            window.attachmentWorker.join();
        const auto pendingConversation = window.Session().CurrentConversationId();
        const bool pendingWasStarted = window.Session().IsStarted();
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), true);
        std::string pendingError;
        expect(window.attachmentRunning.load() && tray->IsEmpty() && !window.StartNewChat(pendingError) &&
                   window.Session().CurrentConversationId() == pendingConversation,
            "completed attachment intake became idle before its queued chips were adopted");
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), pendingWasStarted);
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        expect(tray->Chips().size() == 1 && !tray->Chips().front().thumbnail.isNull() &&
                   window.findChild<QLabel*>("attachmentStatus")->text().contains("first frame", Qt::CaseInsensitive),
            "an animated GIF silently became an attachment without its still-only notice");
        const auto stagedAnimation = revia::runtime::ReviaSessionTestAccess::Attachments(window.Session()).Staged();
        expect(stagedAnimation.size() == 1 && stagedAnimation.front().displayName == "motion.gif" &&
                   stagedAnimation.front().note.find("still image") != std::string::npos,
            "desktop GIF conversion lost its original name or runtime still-image provenance");
        for (const auto& id : tray->Ids())
            window.RemoveAttachmentChip(id);
        window.AttachPaths({animated});
        if (window.attachmentWorker.joinable())
            window.attachmentWorker.join();
        std::string audienceError;
        window.Session().SetAudience({revia::identity::AudienceKind::Shared, "attachment-scope-changed", 0, {"viewer"}}, audienceError);
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        expect(tray->IsEmpty() && revia::runtime::ReviaSessionTestAccess::Attachments(window.Session()).Staged().empty(),
            "an attachment accepted before an audience change published a private thumbnail into the new audience");
        window.Session().SetAudience({revia::identity::AudienceKind::Private, "attach-private", 0, {}}, audienceError);
        tray->SetStatus(QString());
        QImage picture(32, 16, QImage::Format_RGB32);
        picture.fill(Qt::yellow);
        window.attachmentRunning.store(true);
        window.AttachPastedImage(picture);
        expect(tray->IsEmpty(), "pasted pictures can bypass an attachment intake already in progress");
        for (const auto& id : tray->Ids())
            window.RemoveAttachmentChip(id);

        std::string error;
        const bool wasStarted = window.Session().IsStarted();
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), true);
        expect(!window.StartNewChat(error), "a new conversation can replace the origin of unfinished attachment intake");
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), wasStarted);
        window.messageInput->setPlainText("Wait for the picture.");
        window.SendMessage();
        expect(window.messageInput->toPlainText() == "Wait for the picture.",
            "a typed turn can run before its pending picture has become an attachment chip");
        if (window.operationWorker.joinable())
            window.operationWorker.join();
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        window.messageInput->clear();
        window.attachmentRunning.store(false);

        QTemporaryDir chatArchive;
        const auto archivePath = std::filesystem::path(chatArchive.path().toStdWString()) / "attachments-chats.db";
        {
            revia::memory::ConversationArchive seed(archivePath.string());
            expect(seed.BeginSession("attachments-earlier", error), "attachment continuation fixture could not be archived");
        }
        revia::runtime::ReviaSessionTestAccess::OpenConversationArchive(window.Session(), archivePath, "attachments-current");
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), true);
        const auto participant = revia::runtime::ReviaSessionTestAccess::IntroduceTaskSpeaker(window.Session(), "Attachment Owner");
        expect(!participant.empty(), "attachment continuation fixture could not identify its typist");
        {
            revia::memory::ConversationArchive seed(archivePath.string());
            const revia::memory::MemoryScope scope{participant, window.Session().Audience(),
                revia::identity::SpeakerSource::ExplicitIntroduction, 0, window.Session().Stamp().companionId};
            expect(seed.Record("attachments-earlier", "user", "A harmless earlier private chat.", error, scope),
                "attachment continuation fixture could not record its attributed earlier turn");
        }
        window.AttachPastedImage(picture);
        const auto beforeRefusal = tray->Ids();
        window.operationRunning.store(true);
        expect(!window.StartNewChat(error) && tray->Ids() == beforeRefusal, "a refused new chat removed the unsent attachment draft");
        window.viewingArchivedChat = "attachments-earlier";
        window.ContinueArchivedChat();
        expect(tray->Ids() == beforeRefusal && window.Session().CurrentConversationId() == "attachments-current",
            "a refused continuation removed the unsent attachment draft");
        window.operationRunning.store(false);
        expect(window.StartNewChat(error) && tray->IsEmpty() &&
                   revia::runtime::ReviaSessionTestAccess::Attachments(window.Session()).Staged().empty(),
            "a new chat inherited the previous chat's adopted unsent picture");
        window.AttachPastedImage(picture);
        window.viewingArchivedChat = "attachments-earlier";
        window.ContinueArchivedChat();
        expect(window.Session().CurrentConversationId() == "attachments-earlier" && tray->IsEmpty() &&
                   revia::runtime::ReviaSessionTestAccess::Attachments(window.Session()).Staged().empty(),
            "a continued chat inherited the previous chat's adopted unsent picture");
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), wasStarted);
        window.AttachPastedImage(picture);
        expect(tray->Chips().size() == 1, "the outgoing attachment fixture could not stage its picture");
        auto* origin = &window.Session();
        const auto outgoingDirectory = origin->Paths().Resolve("RuntimeData/Attachments/Files");
        auto& library = revia::runtime::ReviaSessionTestAccess::Attachments(*origin);
        const auto keptPreview = library.StageBytes("private-preview.md", "Outgoing companion preview sentinel.\n");
        (void)library.PrepareTurn("preview", library.TakeStaged({keptPreview.attachment.id}), {});
        window.ShowDocuments();
        window.documentsDialog->Select(keptPreview.attachment.id);
        window.documentsDialog->ViewSelected();
        expect(window.documentsDialog->PreviewText().contains("Outgoing companion preview sentinel"),
            "the outgoing document preview fixture was not displayed");

        std::mutex gateMutex;
        std::condition_variable gate;
        bool released = false;
        window.attachmentRunning.store(true);
        window.attachmentWorker = std::jthread(
            [&]()
            {
                std::unique_lock lock(gateMutex);
                gate.wait(lock, [&]() { return released; });
            });
        const auto intakeStop = window.attachmentWorker.get_stop_token();
        revia::runtime::CompanionDescriptor destination;
        expect(window.companions->Create("Attachment lifetime target", "assistant", destination, error),
            "attachment lifetime companion fixture could not be created");
        window.SwitchCompanion(destination.id);
        wait([&]() { return intakeStop.stop_requested() || !window.switchingCompanion; });
        expect(intakeStop.stop_requested() && window.switchingCompanion && &window.Session() == origin,
            "companion replacement retires its attachment session before cancelling and joining intake");
        {
            std::lock_guard lock(gateMutex);
            released = true;
        }
        gate.notify_all();
        wait([&]() { return !window.switchingCompanion; });
        if (window.attachmentWorker.joinable())
            window.attachmentWorker.join();
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        expect(tray->IsEmpty() && window.findChild<QLabel*>("attachmentStatus")->text().isEmpty(),
            "the incoming companion retained outgoing attachment chips or status");
        expect(!window.documentsDialog, "the incoming companion retained a cached outgoing Documents preview");
        std::error_code directoryError;
        std::size_t outgoingFiles = 0;
        for (const auto& file : std::filesystem::directory_iterator(outgoingDirectory, directoryError))
            outgoingFiles += file.is_regular_file() ? 1 : 0;
        expect(outgoingFiles == 1, "retired intake kept its unsent copy or removed a committed document");
        window.SwitchCompanion("legacy");
        wait([&]() { return !window.switchingCompanion; });
        expect(window.Session().ForgetDocument(keptPreview.attachment.id, error), "the retained preview fixture could not be forgotten");
        window.attachmentRunning.store(false);
    }

    static void RunGeneratedCanvasDefaultDenyUi(int& failures)
    {
        QTemporaryDir directory;
        QImage image(32, 16, QImage::Format_RGB32);
        image.fill(Qt::red);
        const QString path = directory.filePath("unverified-generated.png");
        if (!image.save(path))
        {
            ++failures;
            return;
        }
        CanvasPanel panel;
        revia::runtime::RuntimeEvent event;
        event.kind = revia::runtime::RuntimeEventKind::Diagram;
        event.phase = "Image";
        event.resource = path.toStdString();
        conversationMediaReference reference;
        reference.id = "unverified-generated";
        reference.digest = std::string(64, 'a');
        reference.mimeType = "image/png";
        reference.origin = MediaOrigin::GeneratedArtifact;
        event.generatedMediaReference = reference;
        panel.Observe(event);
        if (panel.findChild<QListWidget*>()->count() != 0)
        {
            std::cerr << "FAIL: Canvas trusted a generated path without a current host source resolver.\n";
            ++failures;
        }
    }

    static void RunGeneratedMediaUi(int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        QTemporaryDir directory;
        ReviaWindow window(false, false);
        const auto archivePath = std::filesystem::path(directory.path().toStdWString()) / "generated-ui.db";
        revia::runtime::ReviaSessionTestAccess::OpenConversationArchive(window.Session(), archivePath, "generated-ui");
        const auto participant = revia::runtime::ReviaSessionTestAccess::IntroduceTaskSpeaker(window.Session(), "Generated Media Owner");
        expect(!participant.empty(), "generated continuation fixture could not identify its typist");
        auto& library = revia::runtime::ReviaSessionTestAccess::Attachments(window.Session());
        const auto outputRoot = std::filesystem::path(directory.path().toStdWString()) / "output";
        std::filesystem::create_directory(outputRoot);
        const auto importImage = [&](const char* job, const QColor color)
        {
            QImage pixels(32, 16, QImage::Format_RGB32);
            pixels.fill(color);
            revia::visual::ImageResult artifact;
            artifact.succeeded = true;
            artifact.path = outputRoot / (std::string(job) + ".png");
            expect(pixels.save(QString::fromStdWString(artifact.path.wstring())), "could not encode the generated CPU image fixture");
            QFile file(QString::fromStdWString(artifact.path.wstring()));
            expect(file.open(QIODevice::ReadOnly), "could not read generated CPU image fixture bytes");
            const auto bytes = file.readAll().toStdString();
            file.close();
            auto& expected = artifact.expectedArtifact;
            expected.path = artifact.path;
            expected.jobId = job;
            expected.model = "sd-turbo";
            expected.width = 32;
            expected.height = 16;
            expected.pipeline = "StableDiffusionPipeline";
            expected.variant = "fp16";
            expected.seed = 42;
            expected.requestedSteps = expected.totalInferenceSteps = expected.denoisingSteps = 4;
            auto& receipt = artifact.receipt;
            receipt.path = expected.path;
            receipt.jobId = expected.jobId;
            receipt.model = expected.model;
            receipt.width = expected.width;
            receipt.height = expected.height;
            receipt.provider = expected.provider;
            receipt.pipeline = expected.pipeline;
            receipt.variant = expected.variant;
            receipt.seed = expected.seed;
            receipt.requestedSteps = receipt.totalInferenceSteps = receipt.denoisingSteps = 4;
            receipt.sha256 = revia::audit::ContentDigest(bytes);
            return library.ImportGeneratedImage(artifact, outputRoot, [] { return true; });
        };
        const auto generated = importImage("generated-ui-red", Qt::red);
        expect(generated.succeeded, "strict generated CPU fixture import failed");
        if (!generated.succeeded)
            return;
        const auto present = [&](const conversationMediaReference& reference)
        {
            revia::runtime::SessionResult result;
            result.fromAssistant = true;
            result.text = "A generated CPU fixture is ready.";
            result.stamp = window.Session().Stamp();
            result.audienceRevision = window.Session().Audience().revision;
            result.generatedMediaReferences = {reference};
            window.PresentSessionResult(result);
        };
        present(generated.reference);
        expect(!window.chatEntries.back().generatedMediaReferences.empty() &&
                   !window.chatEntries.back().attachments.front().thumbnail.isNull() &&
                   window.chatHistory->toPlainText().contains("Generated:"),
            "host generated references did not produce a clearly labeled image card");
        const auto cardCount = [&]()
        {
            return std::count_if(window.chatEntries.begin(), window.chatEntries.end(),
                [](const auto& entry) { return !entry.generatedMediaReferences.empty(); });
        };
        const auto beforeForgery = cardCount();
        auto forged = generated.reference;
        forged.digest = std::string(64, 'f');
        present(forged);
        expect(cardCount() == beforeForgery, "a forged digest produced a generated image card");
        revia::runtime::RuntimeEvent event;
        event.kind = revia::runtime::RuntimeEventKind::Diagram;
        event.phase = "Image";
        event.message = "Generated CPU fixture";
        event.resource = "This path must never supply generated pixels.png";
        event.stamp = window.Session().Stamp();
        event.audienceRevision = window.Session().Audience().revision;
        event.conversationMediaEpoch = window.Session().ConversationMediaEpoch();
        event.generatedMediaReference = generated.reference;
        window.HandleRuntimeEvent(event);
        auto* picture = window.canvasPanel->findChild<QLabel*>("canvasImagePreview");
        expect(!picture->pixmap().isNull() && picture->pixmap().toImage().pixelColor(0, 0) == QColor(Qt::red),
            "Canvas did not use verified stored PNG bytes independently of the event's path");
        std::string error;
        expect(window.Session().ForgetDocument(generated.reference.id, error), "generated image could not be forgotten");
        window.RevalidateAttachmentPresentation();
        expect(window.canvasPanel->GeneratedMediaReferences().empty() && picture->pixmap().isNull(),
            "Forget retained a generated Canvas record or cached pixels");
        expect(window.chatEntries.front().body == "A generated CPU fixture is ready." &&
                   window.chatEntries[1].attachments.front().thumbnail.isNull(),
            "generated Forget removed unrelated reply text or retained its thumbnail");

        const auto second = importImage("generated-ui-blue", Qt::blue);
        expect(second.succeeded, "second generated CPU fixture import failed");
        if (!second.succeeded)
            return;
        {
            revia::memory::ConversationArchive archive(archivePath.string());
            const revia::memory::MemoryScope scope{participant, window.Session().Audience(),
                revia::identity::SpeakerSource::ExplicitIntroduction, 0, window.Session().Stamp().companionId};
            expect(archive.BeginSession("generated-earlier", error) &&
                       archive.Record("generated-earlier", "user", "A harmless earlier image request.", error, scope) &&
                       archive.RecordGeneratedAssistant("generated-earlier", "An earlier generated image is ready.", {second.reference},
                           error, scope, [] { return true; }),
                "generated continuation fixture could not record its host-associated earlier turn");
        }
        event.generatedMediaReference = second.reference;
        event.conversationMediaEpoch = window.Session().ConversationMediaEpoch();
        window.HandleRuntimeEvent(event);
        window.Session().Events().Publish(event);
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), true);
        expect(window.StartNewChat(error), "generated-media fixture could not start a new chat");
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        expect(window.canvasPanel->GeneratedMediaReferences().empty() && picture->pixmap().isNull(),
            "a queued old generated event revived pixels in a new conversation");
        window.viewingArchivedChat = "generated-earlier";
        window.ContinueArchivedChat();
        expect(window.Session().CurrentConversationId() == "generated-earlier" && cardCount() == 1 &&
                   !window.chatEntries.back().attachments.front().thumbnail.isNull() &&
                   window.chatHistory->toPlainText().contains("Generated:"),
            "explicit continuation did not restore a verified assistant-generated image card");
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(window.Session(), false);
        expect(window.Session().ForgetDocument(second.reference.id, error),
            "the generated UI fixture could not clean up its retained artifact");
    }

    static void RunForgottenTranscriptMediaUi(int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        QTemporaryDir archiveRoot;
        ReviaWindow window(false, false);
        const auto archive = std::filesystem::path(archiveRoot.path().toStdWString()) / "forget-posters.db";
        revia::runtime::ReviaSessionTestAccess::OpenConversationArchive(window.Session(), archive, "forget-posters");
        auto& library = revia::runtime::ReviaSessionTestAccess::Attachments(window.Session());
        std::vector<AttachmentChip> chips;
        for (const auto color : {Qt::red, Qt::blue, Qt::green})
        {
            QImage picture(32, 16, QImage::Format_RGB32);
            picture.fill(color);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            expect(picture.save(&buffer, "PNG"), "the sent-media cache fixture could not encode its pixels");
            const auto staged = library.StageBytes("sent-poster.png", bytes.toStdString());
            expect(staged.status == revia::attachments::IntakeStatus::Accepted, "the sent-media cache fixture could not stage its image");
            revia::attachments::ImagePolicy policy;
            policy.visionAvailable = true;
            (void)library.PrepareTurn("Keep this private picture.", library.TakeStaged({staged.attachment.id}), policy);
            chips.push_back({staged.attachment.id, QStringLiteral("Sent media"), QStringLiteral("32 x 16"), picture});
        }
        // Image thumbnails and video posters use the same trusted transcript chip/cache path.
        chips.front().video = true;
        chips.front().durationMs = 1000;
        ReviaWindow::ChatEntry entry;
        entry.speaker = QStringLiteral("You");
        entry.body = QStringLiteral("Keep unrelated transcript text: forget-poster-sentinel.");
        entry.userMessage = true;
        entry.attachments = chips;
        window.chatEntries.push_back(std::move(entry));
        window.RenderChat();
        const auto hasPixels = [&](const std::string& id)
        {
            const auto value = window.chatHistory->document()->resource(
                QTextDocument::ImageResource, QUrl(QStringLiteral("revia-attachment:") + QString::fromStdString(id)));
            return value.canConvert<QImage>() && !qvariant_cast<QImage>(value).isNull();
        };
        expect(hasPixels(chips[0].id) && hasPixels(chips[1].id) && hasPixels(chips[2].id) &&
                   window.chatHistory->toHtml().contains(QStringLiteral("video:") + QString::fromStdString(chips[0].id)),
            "the sent-media cache fixture did not render its image resources and playback link");
        QPointer<QTextDocument> oldDocument = window.chatHistory->document();
        window.ShowDocuments();
        QString error;
        expect(window.documentsDialog->ForgetDocument(chips[0].id, error), "the actual Documents Forget callback failed");
        expect(window.chatEntries.back().attachments[0].thumbnail.isNull() && !window.chatEntries.back().attachments[0].video &&
                   !hasPixels(chips[0].id) && !oldDocument &&
                   !window.chatHistory->toHtml().contains(QStringLiteral("video:") + QString::fromStdString(chips[0].id)),
            "Forget retained a sent poster, playback link or old QTextDocument image resource");
        expect(hasPixels(chips[1].id) && hasPixels(chips[2].id) && !window.chatEntries.back().attachments[1].thumbnail.isNull(),
            "forgetting one item cleared an unrelated retained attachment");

        oldDocument = window.chatHistory->document();
        std::string externalError;
        expect(library.Forget(chips[1].id, externalError), "the external owner-revision Forget fixture failed");
        window.RevalidateAttachmentPresentation();
        expect(window.chatEntries.back().attachments[1].thumbnail.isNull() && !hasPixels(chips[1].id) && !oldDocument &&
                   hasPixels(chips[2].id),
            "an owner media revision left a forgotten sent image cached or cleared an unrelated image");

        sqlite3* database = nullptr;
        const bool opened = sqlite3_open(archive.string().c_str(), &database) == SQLITE_OK;
        const bool broken = opened && sqlite3_exec(database, "DROP TABLE conversation_turns;", nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(database);
        expect(broken, "the partial-forget fixture could not force an archive-cleanup failure");
        oldDocument = window.chatHistory->document();
        error.clear();
        expect(!window.documentsDialog->ForgetDocument(chips[2].id, error) && !error.isEmpty() && window.Session().Documents().empty(),
            "the partial-forget fixture did not remove the file before reporting the archive error");
        expect(window.chatEntries.back().attachments[2].thumbnail.isNull() && !hasPixels(chips[2].id) && !oldDocument,
            "a false Forget return after removal retained private sent-image pixels");
        expect(window.chatEntries.back().body == QStringLiteral("Keep unrelated transcript text: forget-poster-sentinel.") &&
                   window.chatEntries.back().attachments.size() == 3 &&
                   window.chatHistory->toPlainText().contains(QStringLiteral("forget-poster-sentinel")),
            "media cache invalidation removed unrelated transcript text or attachment metadata");

        QImage orphan(16, 16, QImage::Format_RGB32);
        orphan.fill(Qt::yellow);
        QByteArray orphanBytes;
        QBuffer orphanBuffer(&orphanBytes);
        orphanBuffer.open(QIODevice::WriteOnly);
        expect(orphan.save(&orphanBuffer, "PNG"), "the orphaned-resource fixture could not encode its image");
        const auto orphaned = library.StageBytes("orphaned-poster.png", orphanBytes.toStdString());
        revia::attachments::ImagePolicy policy;
        policy.visionAvailable = true;
        (void)library.PrepareTurn("Keep an image.", library.TakeStaged({orphaned.attachment.id}), policy);
        window.chatEntries.back().attachments.push_back(
            {orphaned.attachment.id, QStringLiteral("Orphaned poster"), QStringLiteral("16 x 16"), orphan});
        window.RenderChat();
        expect(hasPixels(orphaned.attachment.id), "the orphaned-resource fixture did not register its image");
        window.chatEntries.clear();
        ReviaWindow::ChatEntry textOnly;
        textOnly.speaker = QStringLiteral("You");
        textOnly.body = QStringLiteral("A new transcript keeps this unrelated text.");
        textOnly.userMessage = true;
        window.chatEntries.push_back(std::move(textOnly));
        window.RenderChat();
        expect(hasPixels(orphaned.attachment.id), "the orphaned-resource fixture did not retain the old document cache");
        oldDocument = window.chatHistory->document();
        expect(library.Forget(orphaned.attachment.id, externalError), "the orphaned-resource Forget fixture failed");
        window.RevalidateAttachmentPresentation();
        expect(!hasPixels(orphaned.attachment.id) && !oldDocument &&
                   window.chatHistory->toPlainText().contains(QStringLiteral("A new transcript keeps this unrelated text.")),
            "Forget retained an orphaned document image after its sent chip was removed from the transcript");
    }

    static void RunDocumentImagePreviewUi(ReviaWindow& window, int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        std::vector<revia::attachments::StoredDocument> inventory(1);
        inventory.front().id = "partial-forget";
        inventory.front().name = "partial.md";
        DocumentControls partialControls;
        partialControls.list = [&]() { return inventory; };
        partialControls.preview = [](const std::string&) { return std::string("Preview before a partial forget."); };
        partialControls.forget = [&](const std::string&, std::string& error)
        {
            inventory.clear();
            error = "The record was removed but its stored copy could not be deleted.";
            return false;
        };
        DocumentsDialog partial(std::move(partialControls), &window);
        partial.Select("partial-forget");
        partial.ViewSelected();
        QString error;
        expect(!partial.ForgetDocument("partial-forget", error) && !error.isEmpty() && partial.RowCount() == 0 &&
                   partial.PreviewText().isEmpty(),
            "partial physical deletion left an obsolete document inventory or preview visible");

        QImage picture(32, 16, QImage::Format_RGB32);
        picture.fill(Qt::green);
        QByteArray bytes;
        QBuffer encoded(&bytes);
        encoded.open(QIODevice::WriteOnly);
        expect(picture.save(&encoded, "PNG"), "the private preview fixture could not be encoded");
        auto& library = revia::runtime::ReviaSessionTestAccess::Attachments(window.Session());
        const auto staged = library.StageBytes("preview.png", bytes.toStdString());
        revia::attachments::ImagePolicy policy;
        policy.visionAvailable = true;
        (void)library.PrepareTurn("preview", library.TakeStaged({staged.attachment.id}), policy);
        window.ShowDocuments();
        window.documentsDialog->Select(staged.attachment.id);
        window.documentsDialog->ViewSelected();
        auto* preview = window.documentsDialog->findChild<QLabel*>("documentImagePreview");
        expect(preview && !preview->pixmap().isNull(), "validated image preview did not render its owned bytes");
        const QPointer<DocumentsDialog> privatePreview = window.documentsDialog;
        window.AttachPastedImage(picture);
        std::string audienceError;
        expect(window.Session().SetAudience({revia::identity::AudienceKind::Shared, "preview-room", 0, {"viewer"}}, audienceError),
            "the preview audience fixture could not enter its shared scope");
        QElapsedTimer privacyDeadline;
        privacyDeadline.start();
        while (privatePreview && privacyDeadline.elapsed() < 1500)
        {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(!privatePreview && window.attachmentTray->IsEmpty() && library.Staged().empty(),
            "an audience change retained cached private document pixels or adopted attachment chips");
        window.Session().SetAudience({revia::identity::AudienceKind::Private, "preview-private", 0, {}}, audienceError);
        window.ShowDocuments();
        window.documentsDialog->Select(staged.attachment.id);
        window.documentsDialog->ViewSelected();
        preview = window.documentsDialog->findChild<QLabel*>("documentImagePreview");
        {
            std::ofstream changed(staged.attachment.storedPath, std::ios::binary | std::ios::app);
            changed.put('x');
        }
        window.documentsDialog->ViewSelected();
        expect(preview && preview->pixmap().isNull() && !preview->text().isEmpty(),
            "tampered stored image was rendered through an unchecked cached path");
        std::string forgetError;
        (void)window.Session().ForgetDocument(staged.attachment.id, forgetError);
        window.documentsDialog->Refresh();
        window.documentsDialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    static void RunVisionLive(ReviaWindow& window, const std::filesystem::path& fixtures, const std::filesystem::path& evidence,
        std::ofstream& output, const std::string& secret, const bool videoMode = false, const bool generationMode = false,
        const bool videoGenerationMode = false)
    {
        using Json = nlohmann::json;
        std::mutex outputMutex;
        std::size_t outputBytes = 0;
        const auto record = [&](Json row)
        {
            std::string text = row.dump();
            if (!secret.empty())
                for (std::size_t at = 0; (at = text.find(secret, at)) != std::string::npos; at += 10)
                    text.replace(at, secret.size(), "[redacted]");
            std::lock_guard lock(outputMutex);
            if (outputBytes + text.size() + 1 > 8 * 1024 * 1024)
                return;
            output << text << '\n';
            output.flush();
            outputBytes += text.size() + 1;
        };
        const auto require = [](const bool condition, const char* message)
        {
            if (!condition)
                throw std::runtime_error(message);
        };
        const auto wait = [&](const std::function<bool()>& ready, const char* message)
        {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(generationMode || videoGenerationMode ? 600 : 180);
            while (!ready() && std::chrono::steady_clock::now() < deadline)
            {
                QCoreApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            require(ready(), message);
            if (window.operationWorker.joinable())
                window.operationWorker.join();
            QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        };
        const auto subscription = window.Session().Events().Subscribe(
            [&](const revia::runtime::RuntimeEvent& event)
            {
                if (event.component == "Attachments" || event.component == "Intelligence router" || event.component == "Conversation" ||
                    ((generationMode || videoGenerationMode) &&
                        (event.component == "Canvas" || event.component == "Image generation" || event.component == "Video generation")) ||
                    event.kind == revia::runtime::RuntimeEventKind::Timing || event.kind == revia::runtime::RuntimeEventKind::Error)
                    record({{"kind", "native_event"}, {"component", event.component}, {"phase", event.phase}, {"message", event.message},
                        {"turnId", event.turnId}, {"elapsedMs", event.elapsedMilliseconds}});
            });
        struct Finish
        {
            ReviaWindow& window;
            revia::runtime::RuntimeEventBus::SubscriptionId subscription;
            ~Finish()
            {
                window.Session().RequestStop();
                if (window.attachmentWorker.joinable())
                    window.attachmentWorker.join();
                if (window.operationWorker.joinable())
                    window.operationWorker.join();
                window.Session().Stop();
                window.Session().Events().Unsubscribe(subscription);
            }
        } finish{window, subscription};
        window.resize(1040, 720);
        window.show();
        window.StartRuntime();
        wait([&]() { return !window.operationRunning.load(); }, "The borrowed-server Desktop session did not finish startup.");
        require(window.Session().IsStarted(), "The borrowed-server Desktop session refused startup.");
        record({{"kind", "session_started"}, {"runtimeRoot", window.Session().Paths().InstallRoot().string()},
            {"profile", window.Session().DisplayName()}, {"serverOwnership", "borrowed; not started or stopped by this fixture"}});
        const auto captureChips = [&](const char* phase, const std::size_t expected)
        {
            wait([&]() { return !window.attachmentRunning.load(); }, "Desktop attachment intake did not finish.");
            require(window.attachmentTray->Chips().size() == expected, "The actual UI handler did not produce the expected chips.");
            const auto staged = revia::runtime::ReviaSessionTestAccess::Attachments(window.Session()).Staged();
            require(staged.size() == expected, "The UI chips did not match the actual staged attachments.");
            Json accepted = Json::array();
            for (const auto& item : staged)
            {
                require(videoMode ? (item.kind == revia::attachments::AttachmentKind::Video && item.bytes <= 64 * 1024 * 1024 &&
                                        item.video.durationMs > 0 && item.video.durationMs <= 60000 && item.poster.has_value())
                                  : (item.kind == revia::attachments::AttachmentKind::Image && item.bytes <= 8 * 1024 * 1024),
                    "A live UI attachment exceeded its bounded media contract or lost its poster.");
                const auto stored = evidence / (std::string(phase) + "-" + item.id + item.storedPath.extension().string());
                std::filesystem::copy_file(item.storedPath, stored);
                accepted.push_back(
                    {{"id", item.id}, {"name", item.displayName}, {"digest", item.digest}, {"bytes", item.bytes}, {"width", item.width},
                        {"height", item.height}, {videoMode ? "acceptedClip" : "acceptedPng", stored.filename().string()},
                        {"durationMs", item.video.durationMs}, {"kind", revia::attachments::ToString(item.kind)}});
            }
            record({{"kind", "ui_attachments"}, {"phase", phase}, {"accepted", accepted}});
            SettleLayouts();
            require(window.grab().save(QString::fromStdWString((evidence / (std::string(phase) + ".png")).wstring())),
                "The live UI attachment screenshot could not be saved.");
        };
        const auto submit = [&](const char* phase, const QString& prompt)
        {
            const auto before = window.chatEntries.size();
            window.messageInput->setPlainText(prompt);
            window.SendMessage();
            require(window.messageInput->toPlainText().isEmpty(), "The actual Send handler refused the live request.");
            wait([&]() { return !window.operationRunning.load(); }, "The live UI turn did not finish in its bounded window.");
            Json transcript = Json::array();
            bool answered = false;
            for (std::size_t index = before; index < window.chatEntries.size(); ++index)
            {
                const auto& entry = window.chatEntries[index];
                if (entry.kind != ReviaWindow::EntryKind::Message)
                    continue;
                transcript.push_back(
                    {{"speaker", entry.speaker.toStdString()}, {"body", entry.body.toStdString()}, {"user", entry.userMessage}});
                answered |= !entry.userMessage && entry.markdown && !entry.body.trimmed().isEmpty();
            }
            record({{"kind", "ui_turn"}, {"phase", phase}, {"prompt", prompt.toStdString()}, {"transcript", transcript},
                {"nonemptyAssistantReply", answered}, {"turnError", window.lastTurnError.has_value()}, {"semanticQualified", false}});
            require(answered && !window.lastTurnError.has_value(), "The live UI turn did not deliver an admitted assistant reply.");
            require(window.grab().save(QString::fromStdWString((evidence / (std::string(phase) + "-reply.png")).wstring())),
                "The live UI reply screenshot could not be saved.");
        };
        if (videoGenerationMode)
        {
            auto& library = revia::runtime::ReviaSessionTestAccess::Attachments(window.Session());
            submit("video-generation-owner", "My name is Morgan.");
            submit(
                "video-generation-start", "Create a short video of a red ball rolling slowly across a wooden table, with a fixed camera.");
            wait([&]() { return window.Session().VideoGenerationSnapshot().state == "generating"; },
                "The actual video worker did not enter its generating phase.");
            const auto active = window.Session().VideoGenerationSnapshot();
            const auto began = std::chrono::steady_clock::now();
            submit("video-generation-chat", "Explain in one short sentence why C++ range-for loops are useful.");
            const auto chatMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
            require(chatMs <= 10000, "Ordinary Desktop chat waited behind the active video job.");
            record({{"kind", "video_generation_concurrency"}, {"activeJobId", active.jobId}, {"activeState", active.state},
                {"ordinaryChatMs", chatMs}, {"ordinaryChatLimitMs", 10000}});
            std::optional<conversationMediaReference> selected;
            wait(
                [&]()
                {
                    for (const auto& entry : window.chatEntries)
                        for (const auto& reference : entry.generatedMediaReferences)
                            if (reference.mimeType == "video/mp4" && reference.origin == MediaOrigin::GeneratedArtifact)
                                selected = reference;
                    return selected.has_value();
                },
                "The actual background video job did not publish a typed assistant clip card.");
            require(library.ImageReferencesCurrent({*selected}), "The generated video card is not a current canonical artifact.");
            const auto context = revia::runtime::ReviaSessionTestAccess::WorkingContext(window.Session());
            require(std::any_of(context.begin(), context.end(),
                        [&](const auto& message)
                        {
                            return message.role == "assistant" && std::find(message.mediaReferences.begin(), message.mediaReferences.end(),
                                                                      *selected) != message.mediaReferences.end();
                        }),
                "The generated clip association did not enter actual assistant history.");
            const auto documents = window.Session().Documents();
            const auto document =
                std::find_if(documents.begin(), documents.end(), [&](const auto& item) { return item.id == selected->id; });
            require(document != documents.end() && document->generated && document->digest == selected->digest &&
                        document->generated->mode == "text_to_video" && document->generated->sourceId.empty() &&
                        document->generated->sourceDigest.empty() && selected->width == 384 && selected->height == 256 &&
                        selected->durationMs == 1960,
                "The actual generated video lost its verified mode, source independence, geometry or duration.");
            const auto playback = window.Session().DocumentVideoPreview(selected->id);
            require(playback && playback->reference == *selected, "The generated video card cannot open its validated canonical clip.");
            const auto actualClip = evidence / "video-generation-actual.mp4";
            std::filesystem::copy_file(playback->path, actualClip);
            record({{"kind", "generated_video_artifact"}, {"id", selected->id}, {"digest", selected->digest}, {"name", selected->name},
                {"width", selected->width}, {"height", selected->height}, {"durationMs", selected->durationMs},
                {"mode", document->generated->mode}, {"jobId", document->generated->jobId}, {"provider", document->generated->provider},
                {"model", document->generated->model}, {"modelRevision", document->generated->modelRevision},
                {"seed", document->generated->seed}, {"actualMp4", actualClip.filename().string()}, {"semanticQualified", false}});
            window.OpenVideoPreview(selected->id);
            auto* viewer = dynamic_cast<VideoPreviewDialog*>(window.videoPreviewDialog.data());
            require(viewer != nullptr, "The actual generated video card did not open the trusted player.");
            wait([&]() { return viewer->CurrentTimestampMs() >= 0 && !viewer->IsRunning(); }, "The generated video poster did not render.");
            auto* play = viewer->findChild<QPushButton*>("videoPlayButton");
            auto* pause = viewer->findChild<QPushButton*>("videoPauseButton");
            require(play && pause, "The generated clip player is missing playback controls.");
            play->click();
            wait([&]() { return viewer->CurrentTimestampMs() >= 500; },
                "Generated clip playback did not advance at least 500 milliseconds.");
            require(viewer->QueuedFrameCount() <= 3, "Generated clip playback retained an unbounded queue.");
            pause->click();
            record({{"kind", "generated_video_playback"}, {"timestampMs", viewer->CurrentTimestampMs()},
                {"queuedFrames", viewer->QueuedFrameCount()}, {"silent", true}});
            require(viewer->grab().save(QString::fromStdWString((evidence / "video-generation-playing.png").wstring())),
                "The actual generated player screenshot could not be saved.");
            require(window.grab().save(QString::fromStdWString((evidence / "video-generation-card.png").wstring())),
                "The actual generated video card screenshot could not be saved.");
            submit("video-generation-inspect", "Inspect the generated clip. Describe only what the sampled frames visibly show and give "
                                               "their timestamps. Do not infer continuous motion between samples.");
            std::string error;
            require(window.Session().ForgetDocument(selected->id, error), "The generated clip could not be forgotten.");
            window.RevalidateAttachmentPresentation();
            wait([&]() { return !window.videoPreviewDialog; }, "Forget retained the generated video player.");
            require(!window.Session().DocumentVideoPreview(selected->id) && !library.ImageReferencesCurrent({*selected}),
                "Forget retained canonical generated clip access.");
            for (const auto& entry : window.chatEntries)
                for (const auto& chip : entry.attachments)
                    require(chip.id != selected->id || (!chip.video && chip.thumbnail.isNull()),
                        "Forget retained generated clip card playback or cached poster pixels.");
            require(window.grab().save(QString::fromStdWString((evidence / "video-generation-forgotten.png").wstring())),
                "The generated clip Forget screenshot could not be saved.");
            record({{"kind", "complete"}, {"videoGenerationUiHandlersVerified", true}, {"turns", 4}, {"semanticQualified", false},
                {"qualification",
                    "Actual Send, background generation/import, responsive ordinary chat, typed generated card, trusted playback, visual "
                    "follow-up and Forget. Explicit diagnostic MP4 remains retained; no fake Session result was injected."}});
            return;
        }
        if (generationMode)
        {
            auto& library = revia::runtime::ReviaSessionTestAccess::Attachments(window.Session());
            const auto generated = [&](const char* phase, const std::string_view expectedMode)
            {
                std::optional<conversationMediaReference> selected;
                for (auto entry = window.chatEntries.rbegin(); entry != window.chatEntries.rend() && !selected; ++entry)
                    if (!entry->userMessage && !entry->generatedMediaReferences.empty())
                    {
                        require(
                            entry->generatedMediaReferences.size() == 1, "The image request did not deliver exactly one generated card.");
                        selected = entry->generatedMediaReferences.front();
                    }
                require(selected && selected->origin == MediaOrigin::GeneratedArtifact && library.ImageReferencesCurrent({*selected}),
                    "The actual assistant card has no current native generated image association.");
                const auto pixels = window.Session().DocumentImagePreview(selected->id);
                require(pixels && pixels->bytes && revia::audit::ContentDigest(*pixels->bytes) == selected->digest,
                    "The generated card is not backed by validated canonical image bytes.");
                const auto context = revia::runtime::ReviaSessionTestAccess::WorkingContext(window.Session());
                require(std::any_of(context.begin(), context.end(),
                            [&](const auto& message)
                            {
                                return message.role == "assistant" &&
                                       std::find(message.mediaReferences.begin(), message.mediaReferences.end(), *selected) !=
                                           message.mediaReferences.end();
                            }),
                    "The native generated assistant association did not enter conversation history.");
                const auto canvas = window.canvasPanel->GeneratedMediaReferences();
                auto* preview = window.canvasPanel->findChild<QLabel*>("canvasImagePreview");
                require(std::find(canvas.begin(), canvas.end(), *selected) != canvas.end() && preview && !preview->pixmap().isNull(),
                    "The actual generated image did not reach Canvas pixels.");
                const auto png = evidence / (std::string(phase) + "-actual.png");
                std::ofstream bytes(png, std::ios::binary);
                bytes.write(pixels->bytes->data(), static_cast<std::streamsize>(pixels->bytes->size()));
                bytes.close();
                require(static_cast<bool>(bytes), "The explicit qualification image export failed.");
                const auto documents = window.Session().Documents();
                const auto document =
                    std::find_if(documents.begin(), documents.end(), [&](const auto& item) { return item.id == selected->id; });
                require(document != documents.end() && document->generated && document->digest == selected->digest &&
                            document->generated->mode == expectedMode,
                    "The actual generated card has no persisted provider provenance.");
                record({{"kind", "generated_artifact"}, {"phase", phase}, {"id", selected->id}, {"digest", selected->digest},
                    {"name", selected->name}, {"width", selected->width}, {"height", selected->height},
                    {"actualPng", png.filename().string()}, {"mode", document->generated->mode},
                    {"sourceId", document->generated->sourceId}, {"sourceDigest", document->generated->sourceDigest},
                    {"jobId", document->generated->jobId}, {"provider", document->generated->provider},
                    {"model", document->generated->model}, {"seed", document->generated->seed},
                    {"settingsDigest", document->generated->settingsDigest}, {"semanticQualified", false}});
                return *selected;
            };
            submit("generation-owner", "My name is Morgan.");
            submit("generation-anime",
                "Draw an anime illustration of an adult explorer with teal hair and a red scarf holding a warm lantern. "
                "Use clean line art and cel shading. No text or logos.");
            const auto first = generated("generation-anime", "text_to_image");
            submit(
                "generation-inspect", "Inspect the image you generated. Describe only the visible person, colors, clothing and objects.");
            submit("generation-edit", "Change the image you generated into a watercolor painting while keeping the red scarf.");
            const auto second = generated("generation-edit", "reference_to_image");
            require(first.id != second.id && first.digest != second.digest,
                "The reference edit reused the original artifact instead of producing a distinct output.");
            const auto documents = window.Session().Documents();
            const auto edited = std::find_if(documents.begin(), documents.end(), [&](const auto& item) { return item.id == second.id; });
            require(edited != documents.end() && edited->generated && edited->generated->mode == "reference_to_image" &&
                        edited->generated->sourceId == first.id && edited->generated->sourceDigest == first.digest,
                "The actual reference edit lost the selected original source identity or provider mode.");
            submit("generation-compare",
                QString::fromStdString("Compare " + first.name + " with " + second.name + ". Describe only visible differences."));
            for (const auto& reference : {first, second})
            {
                std::string error;
                require(window.Session().ForgetDocument(reference.id, error), "The actual generated artifact could not be forgotten.");
                window.RevalidateAttachmentPresentation();
                require(!window.Session().DocumentImagePreview(reference.id), "Forget retained canonical generated pixels.");
                for (const auto& entry : window.chatEntries)
                    for (const auto& chip : entry.attachments)
                        require(chip.id != reference.id || chip.thumbnail.isNull(), "Forget retained a generated chat thumbnail.");
                const auto canvas = window.canvasPanel->GeneratedMediaReferences();
                require(std::none_of(canvas.begin(), canvas.end(), [&](const auto& item) { return item.id == reference.id; }),
                    "Forget retained a generated Canvas association.");
                if (reference.id == first.id)
                    require(library.ImageReferencesCurrent({second}),
                        "Forgetting the source silently removed its independently stored edited result.");
            }
            auto* preview = window.canvasPanel->findChild<QLabel*>("canvasImagePreview");
            require(preview && preview->pixmap().isNull(), "Forgetting all generated results retained Canvas pixels.");
            require(window.grab().save(QString::fromStdWString((evidence / "generation-forgotten.png").wstring())),
                "The Forget screenshot could not be saved.");
            record({{"kind", "complete"}, {"generationUiHandlersVerified", true}, {"turns", 5}, {"semanticQualified", false},
                {"generatedClipUiQualified", false},
                {"qualification",
                    "Actual Send, native generation/import, Canvas/card, visual follow-up, reference edit, comparison and Forget. Explicit "
                    "diagnostic PNG exports are retained; no fake Session result or video card was injected."}});
            return;
        }
        if (videoMode)
        {
            window.AttachPaths({QString::fromStdWString((fixtures / "clip-01.mp4").wstring())});
            captureChips("video-attach", 1);
            const auto id = window.attachmentTray->Ids().front();
            submit("video-attach", "Describe the supplied video frames in chronological order. Give each sampled timestamp, its literal "
                                   "label and the two colored shapes. State the limits of this sampling.");
            window.OpenVideoPreview(id);
            auto* viewer = dynamic_cast<VideoPreviewDialog*>(window.videoPreviewDialog.data());
            require(viewer != nullptr, "The transcript video did not open the trusted local player.");
            wait([&]() { return viewer->CurrentTimestampMs() >= 0 && !viewer->IsRunning(); },
                "The local video poster did not finish decoding and rendering.");
            require(viewer->grab().save(QString::fromStdWString((evidence / "video-player-poster.png").wstring())),
                "The video player poster capture could not be saved.");
            auto* play = viewer->findChild<QPushButton*>("videoPlayButton");
            require(play != nullptr, "The local player did not expose its Play control.");
            play->click();
            wait([&]() { return viewer->CurrentTimestampMs() >= 500; }, "Local playback did not advance its timestamp.");
            require(viewer->QueuedFrameCount() <= 3, "Local playback retained an unbounded frame queue.");
            record({{"kind", "ui_video_playback"}, {"timestampMs", viewer->CurrentTimestampMs()},
                {"queuedFrames", viewer->QueuedFrameCount()}, {"silent", true}, {"scope", "trusted local clip"}});
            require(viewer->grab().save(QString::fromStdWString((evidence / "video-player-playing.png").wstring())),
                "The playing video capture could not be saved.");
            window.CloseVideoPreview();
            submit("video-followup", "Inspect 00:11.500 to 00:12.500 in that clip. Read the literal label and identify what appears in the "
                                     "center at each sampled timestamp. Do not infer events between frames.");
            QMimeData dropped;
            dropped.setUrls({QUrl::fromLocalFile(QString::fromStdWString((fixtures / "clip-02.webm").wstring()))});
            QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
            QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window.messageInput->viewport(), &enter);
            QCoreApplication::sendEvent(window.messageInput->viewport(), &drop);
            require(enter.isAccepted() && drop.isAccepted(), "The actual drag/drop handler refused a supported local video.");
            captureChips("video-drop", 1);
            submit("video-drop", "Inspect 00:05 to 00:06 in the attached clip. Read its literal label and describe the two colored shapes.");
            record({{"kind", "complete"}, {"videoUiHandlersVerified", true}, {"turns", 3}, {"semanticQualified", false},
                {"qualification", "Await independent clip/frame wire and answer review; actual Attach, Drop, Send and local playback ran."}});
            return;
        }
        window.AttachPaths({QString::fromStdWString((fixtures / "asset-01.png").wstring())});
        captureChips("attach", 1);
        submit("attach", "Describe the picture I attached. Include its shapes, colors, positions and exact visible text.");
        submit("followup", "Look at that image again. What is the exact text visible in it?");
        const QImage pasted(QString::fromStdWString((fixtures / "asset-02.png").wstring()));
        require(!pasted.isNull(), "The clipboard image fixture could not be decoded.");
        QApplication::clipboard()->setImage(pasted);
        QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
        QCoreApplication::sendEvent(window.messageInput, &paste);
        require(window.attachmentTray->Chips().size() == 1, "The actual clipboard handler did not attach the picture.");
        QMimeData dropped;
        dropped.setUrls({QUrl::fromLocalFile(QString::fromStdWString((fixtures / "asset-01.png").wstring()))});
        QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
        QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window.messageInput->viewport(), &enter);
        QCoreApplication::sendEvent(window.messageInput->viewport(), &drop);
        require(enter.isAccepted() && drop.isAccepted(), "The actual drag/drop handler did not accept the file.");
        captureChips("paste-drop", 2);
        submit("paste-drop", "Compare only the two pictures attached to this message. For each picture give its exact visible text, "
                             "shape colors and their positions. State their differences.");
        record({{"kind", "complete"}, {"uiHandlersVerified", true}, {"turns", 3}, {"semanticQualified", false},
            {"qualification", "Await independent exact-wire image audit and semantic review of these actual replies."}});
    }

    static void RunRegenerateAndEditUi(ReviaWindow& window, int& failures)
    {
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        const auto finish = [&]()
        {
            if (window.operationWorker.joinable())
                window.operationWorker.join();
            QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        };
        const QString revia = QString::fromStdString(window.Session().DisplayName());
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.AppendChat("You", "Original question", true);
        window.AppendChat(revia, "Original **reply**.");
        QString shown = window.chatHistory->toHtml();
        expect(shown.contains("href=\"regen:1\"") && shown.contains("href=\"edit:0\""),
            "the latest exchange has no Regenerate and Edit links");

        // The session refuses (it is not started here), so the transcript is put back.
        emit window.chatHistory->anchorClicked(QUrl("regen:1"));
        finish();
        expect(window.chatEntries.size() == 2 && window.chatEntries.back().body == "Original **reply**." &&
                   window.statusBar()->currentMessage().contains("not ready"),
            "a refused regenerate did not restore the reply or say why");

        emit window.chatHistory->anchorClicked(QUrl("edit:0"));
        expect(window.editingLastTurn && window.messageInput->toPlainText() == "Original question", "Edit did not load the message");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(window.messageInput, &escape);
        expect(!window.editingLastTurn && window.messageInput->toPlainText().isEmpty(), "Esc did not cancel the edit");

        emit window.chatHistory->anchorClicked(QUrl("edit:0"));
        window.messageInput->setPlainText("Edited question");
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(window.messageInput, &enter);
        expect(window.chatEntries.size() == 1 && window.chatEntries.front().body == "Edited question",
            "sending an edit did not replace the last exchange on screen");
        finish();
        expect(window.chatEntries.size() == 2 && window.chatEntries.front().body == "Original question" &&
                   window.chatEntries.back().body == "Original **reply**." && !window.editingLastTurn,
            "a refused edit did not restore the original exchange");

        // A command result after the reply leaves nothing to regenerate, and reply text
        // cannot supply the link itself.
        window.AppendChat("System", "Command output [regen](regen:1)");
        shown = window.chatHistory->toHtml();
        expect(!shown.contains("href=\"regen:") && shown.contains("href=\"edit:0\""), "Regenerate was offered for a command result");
        emit window.chatHistory->anchorClicked(QUrl("regen:1"));
        expect(!window.operationRunning.load() && window.chatEntries.size() == 3, "a stale Regenerate link still acted");
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.RenderChat();
    }

    static void RunDelegationCardUi(ReviaWindow& window, int& failures)
    {
        using namespace revia::runtime;
        const auto expect = [&](const bool condition, const char* message)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << message << '\n';
                ++failures;
            }
        };
        // Synthetic session events only: no model runs, and this session never started, so
        // it reports on no delegated task.
        const auto delegation = [&](const std::string& phase, const std::string& id, const std::uint64_t turn, const std::string& message)
        {
            RuntimeEvent event{RuntimeEventKind::AgentWorkflow, RuntimeState::Thinking, message};
            event.component = "Delegation";
            event.phase = phase;
            event.detail = id;
            event.conversationTurnId = turn;
            window.HandleRuntimeEvent(event);
        };
        const auto cards = [&]()
        {
            return std::count_if(window.chatEntries.begin(), window.chatEntries.end(),
                [](const auto& entry) { return entry.kind == ReviaWindow::EntryKind::DelegatedTask; });
        };
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.AppendChat("You", "Find out why the build is slow.", true);

        // The submission arrives mid-turn, before the reply it belongs under.
        delegation("Submitted", "fixture-task-a", 5101, "Find why the <b>build</b> is slow");
        expect(window.heldTaskCard.has_value() && cards() == 0, "a delegation card was drawn before the reply that delegated");
        RuntimeEvent reply{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "I handed that to my agents."};
        reply.audienceRevision = window.Session().Audience().revision;
        window.HandleRuntimeEvent(reply);
        expect(!window.heldTaskCard && window.chatEntries.size() == 3 && window.chatEntries[1].body == "I handed that to my agents." &&
                   window.chatEntries.back().kind == ReviaWindow::EntryKind::DelegatedTask,
            "the held delegation card was not appended directly beneath the reply");
        delegation("Submitted", "fixture-task-a", 5101, "Find why the build is slow");
        expect(cards() == 1, "a repeated submission added a second card for one task");

        // Hiding work summaries must not hide the card, and a card whose task the session no
        // longer reports on says so and offers no Cancel.
        window.showWorkSummaries = false;
        window.RenderChat();
        const QString html = window.chatHistory->toHtml();
        const QString text = window.chatHistory->toPlainText();
        window.showWorkSummaries = true;
        expect(text.contains("Revia's agents") && text.contains("No longer tracked") && !text.contains("Revia is thinking"),
            "the task card was hidden, mislabelled, or claimed a task the session does not report");
        expect(html.contains("href=\"task-details:2\"") && !html.contains("href=\"task-cancel:"),
            "a card whose task is not current offered Cancel");
        expect(text.contains("<b>build</b>") && !html.contains("<b>build"), "the delegated objective was interpreted as markup");
        expect(html.contains("href=\"copy:1\"") && html.contains("href=\"regen:1\"") && html.contains("href=\"edit:0\""),
            "the card broke Copy, Regenerate or Edit for the exchange above it");

        // A task link acts only on a card, and a card's Cancel names its own task.
        window.statusBar()->clearMessage();
        emit window.chatHistory->anchorClicked(QUrl("task-cancel:1"));
        expect(window.statusBar()->currentMessage().isEmpty(), "a task link acted on an entry that is not a task card");
        emit window.chatHistory->anchorClicked(QUrl("task-cancel:2"));
        expect(window.statusBar()->currentMessage().contains("No delegated task"), "a stale card's Cancel was not refused by its task id");
        window.chatTurnInFlight = true;
        emit window.chatHistory->anchorClicked(QUrl("task-details:2"));
        auto* studio = window.findChild<QScrollArea*>("agentStudioScroll");
        auto* studioWindow = window.panelHost->Window(revia::desktop::PanelId::Agents);
        expect(studio && studioWindow && studioWindow->Page() == studio && window.panelHost->IsOpen(revia::desktop::PanelId::Agents),
            "the card's Details did not open the Agents panel while a reply was in flight");
        window.panelHost->Close(revia::desktop::PanelId::Agents);
        window.chatTurnInFlight = false;

        // A streamed reply is already on screen when its turn ends; the card goes beneath it.
        delegation("Submitted", "fixture-task-b", 5102, "Second objective");
        RuntimeEvent fragment{RuntimeEventKind::ReplyFragment, RuntimeState::Responding, "Streamed hand-off reply."};
        fragment.audienceRevision = window.Session().Audience().revision;
        fragment.conversationTurnId = 5102;
        window.HandleRuntimeEvent(fragment);
        expect(window.heldTaskCard.has_value(), "a card was placed before its streamed turn ended");
        RuntimeEvent ended{RuntimeEventKind::ComponentStatus, RuntimeState::Thinking, "Turn #5102 completed."};
        ended.component = "Conversation";
        ended.phase = "Ready";
        ended.turnId = 5102;
        window.HandleRuntimeEvent(ended);
        expect(!window.heldTaskCard && window.chatEntries.back().delegated.workflowId == "fixture-task-b" &&
                   window.chatEntries[window.chatEntries.size() - 2].body == "Streamed hand-off reply.",
            "the card was not placed beneath the streamed reply when its turn ended");
        // A submission seen only after its turn ended has no reply left to wait for.
        delegation("Submitted", "fixture-task-c", 5102, "Late objective");
        expect(!window.heldTaskCard && window.chatEntries.back().delegated.workflowId == "fixture-task-c",
            "a submission after its turn ended was held instead of shown");
        delegation("Submitted", "fixture-task-d", 5200, "Discarded objective");
        window.ResetConversationPresentation();
        expect(!window.heldTaskCard, "a held card outlived the conversation presentation it belonged to");

        auto* origin = window.findChild<QLabel*>("agentDelegation");
        auto* mode = window.findChild<QComboBox*>("delegationModeCombo");
        expect(origin && origin->isHidden(), "Agent Studio claimed a chat origin with no delegated task");
        expect(mode && mode->count() == 3 && mode->itemData(0).toString() == "off" && mode->itemData(1).toString() == "on_request" &&
                   mode->itemData(2).toString() == "auto" && !mode->toolTip().isEmpty(),
            "the delegation mode control is missing its three exact settings or its explanation");
        expect(mode && window.settingsDialog->SectionPage(revia::desktop::SettingsSection::Chat)->isAncestorOf(mode) &&
                   window.settingsDialog->findChild<QWidget*>("delegationModeRow") != nullptr,
            "the delegation choice was not hosted in the Chat section of Settings");
        window.chatEntries.clear();
        window.ResetConversationPresentation();
        window.RenderChat();
    }

    static void Run(ReviaWindow& window, int& failures)
    {
        using revia::runtime::RuntimeEvent;
        using revia::runtime::RuntimeEventKind;
        using revia::runtime::RuntimeState;
        window.pollTimer->stop();
        const auto expectStop = [&](const bool expected, const char* context)
        {
            if (window.stopButton->isEnabled() != expected)
            {
                std::cerr << "FAIL: " << context << '\n';
                ++failures;
            }
        };
        const auto state = [&](const RuntimeState value)
        {
            RuntimeEvent event;
            event.kind = RuntimeEventKind::StateChanged;
            event.state = value;
            window.HandleRuntimeEvent(event);
        };
        const auto voice = [&](const std::string& phase)
        {
            RuntimeEvent event;
            event.kind = RuntimeEventKind::ComponentStatus;
            event.component = "Voice";
            event.phase = phase;
            window.HandleRuntimeEvent(event);
        };

        for (const auto backgroundState : {RuntimeState::Starting, RuntimeState::Thinking, RuntimeState::Responding, RuntimeState::Acting})
        {
            state(backgroundState);
            expectStop(true, "active runtime state must allow Stop");
            window.ApplyMicrophoneUi(ReviaWindow::MicrophoneUi::Ready);
            expectStop(true, "microphone refresh must preserve background cancellation");
            voice("Stopped");
            expectStop(true, "voice completion must preserve background cancellation");
        }

        state(RuntimeState::Acting);
        const auto previousEntries = window.chatEntries.size();
        window.messageInput->setPlainText("A foreground request completed.");
        window.SendMessage();
        window.operationWorker.join();
        // The nonstarted session returns immediately without running a model. Its
        // real queued result callback must preserve the existing Acting state.
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        if (window.chatEntries.size() != previousEntries + 2)
        {
            std::cerr << "FAIL: the actual foreground completion callback did not display its result\n";
            ++failures;
        }
        expectStop(true, "foreground completion must preserve background cancellation");

        state(RuntimeState::Idle);
        expectStop(false, "idle runtime with no audio activity must disable Stop");
        window.ApplyMicrophoneUi(ReviaWindow::MicrophoneUi::Listening);
        expectStop(true, "microphone recording must allow Stop");
        voice("Stopped");
        expectStop(true, "voice completion must preserve microphone cancellation");
        window.ApplyMicrophoneUi(ReviaWindow::MicrophoneUi::Ready);
        expectStop(false, "completed microphone activity must disable Stop when idle");
        voice("Speaking");
        expectStop(true, "speech playback must allow Stop");
        const auto beforeFaultText = window.chatEntries.size();
        RuntimeEvent fragment;
        fragment.kind = RuntimeEventKind::ReplyFragment;
        fragment.audienceRevision = window.Session().Audience().revision;
        fragment.message = "Approved text before synthesis completes.";
        fragment.turnId = 901;
        window.HandleRuntimeEvent(fragment);
        RuntimeEvent fault;
        fault.kind = RuntimeEventKind::ComponentStatus;
        fault.component = "Voice health";
        fault.phase = "Degraded";
        fault.message = "Voice synthesis failed for this reply. The text remains available. The cause is unknown.";
        window.HandleRuntimeEvent(fault);
        expectStop(true, "synthesis health must preserve active playback cancellation");
        RuntimeEvent text;
        text.kind = RuntimeEventKind::AssistantMessage;
        text.audienceRevision = window.Session().Audience().revision;
        text.message = "Approved text after synthesis failed.";
        text.turnId = 902;
        window.HandleRuntimeEvent(text);
        if (window.chatEntries.size() != beforeFaultText + 2 || !window.pendingUtterances.empty())
        {
            std::cerr << "FAIL: approved reply text was held for failed speech or lost in the health update\n";
            ++failures;
        }
        window.ApplyMicrophoneUi(ReviaWindow::MicrophoneUi::Ready);
        expectStop(true, "microphone refresh must preserve speech cancellation");
        voice("Stopped");
        expectStop(false, "completed speech must disable Stop when idle");

        std::mutex heldTaskMutex;
        std::condition_variable_any heldTaskCondition;
        std::string launchMessage;
        const bool launched = revia::runtime::ReviaSessionTestAccess::LaunchTask(
            window.session, "desktop cancellation fixture",
            [&](const std::stop_token stop)
            {
                std::unique_lock lock(heldTaskMutex);
                heldTaskCondition.wait(lock, stop, [] { return false; });
                revia::goals::Goal result;
                result.title = "desktop cancellation fixture";
                result.status = revia::goals::GoalStatus::Cancelled;
                return result;
            },
            launchMessage);
        if (!launched)
        {
            std::cerr << "FAIL: background task did not launch: " << launchMessage << '\n';
            ++failures;
            return;
        }
        for (const auto foregroundState : {RuntimeState::Remembering, RuntimeState::Blocked, RuntimeState::Error})
        {
            state(foregroundState);
            expectStop(true, "held task must remain cancellable through foreground states");
            window.ApplyMicrophoneUi(ReviaWindow::MicrophoneUi::Ready);
            expectStop(true, "microphone refresh must preserve held-task cancellation");
            voice("Stopped");
            expectStop(true, "voice completion must preserve held-task cancellation");
        }
        window.session.RequestStop();
        revia::runtime::ReviaSessionTestAccess::WaitForTask(window.session);
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        expectStop(false, "finished background task must disable Stop when idle");
    }
};

namespace
{
// The suite this binary has always run: one ReviaWindow taken through every regression above.
int RunPolicySuite(int, char**)
{
    int failures = 0;
    {
        ReviaWindow window(false, false);
        ReviaWindowStopTests::Run(window, failures);
        ReviaWindowStopTests::RunChatTranscriptUi(window, failures);
        ReviaWindowStopTests::RunChatSessionsUi(window, failures);
        ReviaWindowStopTests::RunRegenerateAndEditUi(window, failures);
        ReviaWindowStopTests::RunDelegationCardUi(window, failures);
        ReviaWindowStopTests::RunAttachmentUi(window, failures);
        ReviaWindowStopTests::RunAttachmentLifecycleUi(window, failures);
        ReviaWindowStopTests::RunDocumentImagePreviewUi(window, failures);
        ReviaWindowStopTests::RunForgottenTranscriptMediaUi(failures);
        ReviaWindowStopTests::RunConfirmationAudienceUi(window, failures);
        ReviaWindowStopTests::RunConversationQualityUi(window, failures);
        ReviaWindowStopTests::RunMemoryRevisionUi(window, failures);
        ReviaWindowStopTests::RunMemoryCorrectionProposalUi(window, failures);
        ReviaWindowStopTests::RunShellUi(window, failures);
        ReviaWindowStopTests::RunSettingsReadBackUi(window, failures);
        ReviaWindowStopTests::RunHealthUiAndLayout(window, failures);
        ReviaWindowStopTests::RunCompanionStudio(window, failures);
    }
    // Last, on a window of its own: it ends that window's event loop for good.
    ReviaWindowStopTests::RunShutdownUi(failures);
    if (failures == 0)
        std::cout << "Desktop Stop policy regression passed.\n";
    return failures;
}
}

REVIA_DESKTOP_SUITE("policy", RunPolicySuite);

namespace
{
int RunAttachmentSuite(int, char**)
{
    int failures = 0;
    ReviaWindow window(false, false);
    ReviaWindowStopTests::RunAttachmentUi(window, failures);
    ReviaWindowStopTests::RunAttachmentLifecycleUi(window, failures);
    ReviaWindowStopTests::RunDocumentImagePreviewUi(window, failures);
    ReviaWindowStopTests::RunForgottenTranscriptMediaUi(failures);
    ReviaWindowStopTests::RunGeneratedCanvasDefaultDenyUi(failures);
    ReviaWindowStopTests::RunGeneratedMediaUi(failures);
    return failures;
}
}

REVIA_DESKTOP_SUITE("attachments", RunAttachmentSuite);

namespace
{
int RunAttachmentForgetSuite(int, char**)
{
    int failures = 0;
    ReviaWindowStopTests::RunForgottenTranscriptMediaUi(failures);
    return failures;
}
}

REVIA_DESKTOP_SUITE("attachment-forget", RunAttachmentForgetSuite);

namespace
{
int RunVisionLiveSuite(int argc, char** argv)
{
    std::ofstream output;
    std::string secret;
    try
    {
        const auto argument = [&](const std::string_view prefix)
        {
            for (int index = 1; index < argc; ++index)
                if (const std::string_view value = argv[index]; value.starts_with(prefix))
                    return std::filesystem::absolute(std::filesystem::u8path(value.substr(prefix.size())));
            throw std::runtime_error("vision-live requires --vision-config-root=, --vision-fixtures= and --vision-evidence=.");
        };
        const auto configRoot = std::filesystem::canonical(argument("--vision-config-root="));
        const auto fixtures = std::filesystem::canonical(argument("--vision-fixtures="));
        const auto evidence = argument("--vision-evidence=");
        if (std::filesystem::exists(evidence) || !std::filesystem::create_directory(evidence))
            throw std::runtime_error("Choose a new evidence directory under an existing parent; prior evidence is never replaced.");
        output.open(evidence / "desktop-vision-live.jsonl", std::ios::binary);
        if (!output.is_open())
            throw std::runtime_error("The fresh Desktop vision report could not be opened.");
        const auto destination = revia::core::RuntimeRoot() / "Config";
        std::uintmax_t total = 0;
        std::size_t files = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(configRoot / "Config"))
        {
            if (entry.is_symlink() || (!entry.is_directory() && !entry.is_regular_file()))
                throw std::runtime_error("The disposable Config must contain ordinary directories and files only.");
            const auto target = destination / std::filesystem::relative(entry.path(), configRoot / "Config");
            if (entry.is_directory())
                std::filesystem::create_directories(target);
            else
            {
                const auto bytes = entry.file_size();
                total += bytes;
                if (bytes > 1024 * 1024 || total > 16 * 1024 * 1024 || ++files > 128)
                    throw std::runtime_error("The disposable Config exceeds its bounded fixture limits.");
                std::filesystem::copy_file(entry.path(), target);
            }
        }
        configManager config((destination / "settings.json").string(), (destination / "Profiles").string());
        appSettings settings;
        aiProfile profile;
        if (!config.LoadSettings(settings) || !config.LoadProfile(settings.activeProfile, profile))
            throw std::runtime_error("The disposable Desktop settings/profile did not load.");
        secret = settings.llm.apiKey;
        const bool videoMode =
            std::any_of(argv + 1, argv + argc, [](const char* value) { return std::string_view(value) == "--video-mode"; });
        const bool generationMode =
            std::any_of(argv + 1, argv + argc, [](const char* value) { return std::string_view(value) == "--generation-mode"; });
        const bool videoGenerationMode =
            std::any_of(argv + 1, argv + argc, [](const char* value) { return std::string_view(value) == "--video-generation-mode"; });
        if (static_cast<int>(videoMode) + static_cast<int>(generationMode) + static_cast<int>(videoGenerationMode) > 1)
            throw std::runtime_error("Choose one live UI media mode.");
        const float temperature = profile.bHasTemperatureOverride ? profile.temperature : settings.llm.temperature;
        if (settings.llm.backend != "LLamaCpp" || settings.llm.host != "127.0.0.1" || settings.llm.bAutoStartServer ||
            settings.llm.bShutdownServerOnExit || !settings.llm.bVisionEnabled || settings.llm.cloud.bEnabled ||
            settings.llm.contextSize != 32768 || settings.llm.imageMinTokens != 1024 || std::abs(temperature - 0.75F) > 0.001F)
            throw std::runtime_error(
                "Use the borrowed local Main vision1024/context32768/profile.75 server, with automatic start/shutdown off.");
        if (profile.bMemoryEnabled || settings.speech.bEnabled || settings.speechRecognition.bEnabled || settings.embedding.bEnabled ||
            settings.intelligence.bEnabled || settings.vision.bEnabled || settings.vision.bContinuousAwareness ||
            settings.perception.bEnabled || settings.initiative.bEnabled || settings.image.bEnabled != generationMode ||
            settings.video.bEnabled != videoGenerationMode || settings.performance.bEnabled || settings.improvement.bEnabled ||
            settings.presence.bEnabled || settings.presence.bAvatarBridgeEnabled || settings.presence.bExternalAdaptersEnabled ||
            settings.localApi.bEnabled || !settings.mcp.servers.empty())
            throw std::runtime_error("Disable auxiliary models, memory, audio, screenshots, app awareness, initiative, adapters and MCP; "
                                     "generation is allowed only in its matching opt-in mode.");
        if (generationMode &&
            (settings.image.model != "stabilityai/sd-turbo" || settings.image.device != "cuda:1" || settings.image.width != 512 ||
                settings.image.height != 512 || settings.image.steps != 4 || settings.image.seed != 42 || !settings.image.bOffline ||
                settings.image.bKeepLoaded || !settings.image.bShutdownOnExit || settings.image.host != "127.0.0.1"))
            throw std::runtime_error("Generation UI qualification requires offline SD-Turbo/cuda1/512px/4steps/seed42 with owned release.");
        if (videoGenerationMode &&
            (settings.video.modelDirectory.empty() || settings.video.runtimeOverlayDirectory.empty() || settings.video.width != 384 ||
                settings.video.height != 256 || settings.video.frameCount != 49 || settings.video.fps != 25 || settings.video.steps != 50 ||
                settings.video.gpuIndex != 1 || settings.video.cpuThreads != 6 || settings.video.requestTimeoutSeconds != 600))
            throw std::runtime_error(
                "Video generation UI qualification requires the approved LTX secondary-GPU/384x256/49frames/25fps/50steps/CPU6 route.");
        for (const char* name : generationMode || videoGenerationMode ? std::initializer_list<const char*>{}
                                : videoMode                           ? std::initializer_list<const char*>{"clip-01.mp4", "clip-02.webm"}
                                                                      : std::initializer_list<const char*>{"asset-01.png", "asset-02.png"})
            if (!std::filesystem::is_regular_file(fixtures / name) ||
                std::filesystem::file_size(fixtures / name) > (videoMode ? 64 : 8) * 1024 * 1024)
                throw std::runtime_error("Two bounded neutral media fixtures are required.");
        output << nlohmann::json{{"kind", "preflight"}, {"contextSize", settings.llm.contextSize},
                      {"imageMinTokens", settings.llm.imageMinTokens}, {"temperature", temperature}, {"model", settings.llm.modelName},
                      {"port", settings.llm.port}, {"fixtureRoot", fixtures.string()}, {"generationMode", generationMode},
                      {"videoGenerationMode", videoGenerationMode}, {"imageGenerationEnabled", settings.image.bEnabled},
                      {"videoGenerationEnabled", settings.video.bEnabled}, {"imageGenerationModel", settings.image.model},
                      {"imageGenerationDevice", settings.image.device}, {"imageGenerationSeed", settings.image.seed},
                      {"semanticQualified", false}}
                      .dump()
               << '\n';
        output.flush();
        ReviaWindow window(false, false);
        ReviaWindowStopTests::RunVisionLive(window, fixtures, evidence, output, secret, videoMode, generationMode, videoGenerationMode);
        if (!output.good())
            throw std::runtime_error("Desktop vision evidence could not be saved completely.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::string message = error.what();
        if (!secret.empty())
            for (std::size_t at = 0; (at = message.find(secret, at)) != std::string::npos; at += 10)
                message.replace(at, secret.size(), "[redacted]");
        if (output.is_open())
            output << nlohmann::json{{"kind", "failed"}, {"message", message}, {"semanticQualified", false}}.dump() << '\n';
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }
}
}

// Explicit opt-in only: this suite borrows an already-owned server and is never a normal CTest.
REVIA_DESKTOP_SUITE("vision-live", RunVisionLiveSuite);

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    // --suite=<name> picks one of the suites linked into this binary. CTest treats 77 as skipped,
    // which is what a suite whose source is not built in yet reports.
    std::string suiteName = "policy";
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument.starts_with("--suite="))
            suiteName = argument.substr(std::string_view("--suite=").size());
    }
    const auto suite = revia::tests::DesktopSuites().find(suiteName);
    if (suite == revia::tests::DesktopSuites().end())
    {
        std::cout << "suite " << suiteName << " is not built into this binary\n";
        return 77;
    }
    // Offscreen Qt needs an explicit font; its Windows font discovery is unavailable.
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/segoeui.ttf");
    application.setFont(QFont("Segoe UI"));
    QTemporaryDir settings;
    if (!settings.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QApplication::setOrganizationName("ReviaTests");
    QApplication::setApplicationName("DesktopStopPolicy");
    const QString previousDirectory = QDir::currentPath();
    if (!QDir().mkpath(settings.path() + "/Config") || !QDir::setCurrent(settings.path()))
        return 2;
    struct RestoreDirectory
    {
        QString previous;
        ~RestoreDirectory()
        {
            QDir::setCurrent(previous);
        }
    } restore{previousDirectory};
    if (revia::core::RuntimeRoot() != std::filesystem::path(settings.path().toStdWString()))
        return 2;
    return suite->second(argc, argv) == 0 ? 0 : 1;
}
