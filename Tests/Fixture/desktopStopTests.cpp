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
#include "chatMarkdown.h"
#include "chatSessionsPanel.h"
#include "documentsDialog.h"
#include "Memory/conversationArchive.h"
#include "memoryPanel.h"
#include "Memory/longTermMemory.h"
#include "Audit/contentDigest.h"
#include "Policy/desktopAuthorization.h"
#include "reviaSessionTestAccess.h"
#include "Core/runtimePath.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
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
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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
        window.learningStudioPanel->SetSnapshot(learning);
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
            window.session.Events().Publish(event);
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
            window.session.Events().Publish(cue);
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
        window.OpenSettings("Models");
        SettleLayouts();
        expect(window.cloudUseForCombo->focusPolicy() == Qt::StrongFocus && window.initiativeMaxSpin->focusPolicy() == Qt::StrongFocus &&
                   window.resourceSampleSpin->focusPolicy() == Qt::StrongFocus,
            "the window's Settings controls must not change under a passing wheel");
        const QString savedUse = window.cloudUseForReader ? window.cloudUseForReader("llm.cloud.useFor") : QString();
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
