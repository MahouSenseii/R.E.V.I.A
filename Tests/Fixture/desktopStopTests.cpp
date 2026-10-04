#include "reviaWindow.h"
#include "tabNavigation.h"
#include "agentStudioPanel.h"
#include "audienceStudioPanel.h"
#include "developmentStudioPanel.h"
#include "learningStudioPanel.h"
#include "answerFeedbackDialog.h"
#include "memoryPanel.h"
#include "Memory/longTermMemory.h"
#include "Policy/desktopAuthorization.h"
#include "reviaSessionTestAccess.h"
#include "Core/runtimePath.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegion>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <mutex>
#include <string>
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
            window.resize(size);
            window.agentStudioPanel->SetSnapshot(window.Session().AgentWorkflowSnapshot());
            SettleLayouts();
            const int bottom = details->mapTo(scroll->widget(), QPoint(0, 0)).y() + details->height();
            scroll->verticalScrollBar()->setValue(std::max(0, bottom - scroll->viewport()->height()));
            SettleLayouts();
            const auto name = QString("agent-studio-%1-details-%2x%3.png").arg(phase).arg(size.width()).arg(size.height());
            if (!window.grab().save(QDir(renderDirectory).filePath(name)))
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
            const auto outcome = runtime.Execute(request, true);
            expect(outcome.Succeeded() == allowed && std::filesystem::exists(request.source) == allowed,
                "companion selection lost its denial or narrowed the other companion");
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
        window.SwitchCompanion(second.id);
        expect(!window.tabs->isEnabled(), "mutable companion controls remain enabled while the outgoing runtime retires");
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
        window.SwitchCompanion("legacy");
        wait([&]() { return !window.switchingCompanion; });
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
        provider->setCurrentIndex(1);
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
        revia::desktop::SelectNavigationPage(window.findChild<QScrollArea*>("agentStudioScroll"));
        for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
        {
            window.resize(size);
            window.agentStudioPanel->SetSnapshot(window.Session().AgentWorkflowSnapshot());
            SettleLayouts();
            auto* scroll = window.findChild<QScrollArea*>("agentStudioScroll");
            expect(scroll && scroll->horizontalScrollBar()->maximum() == 0, "live Agent Studio overflows horizontally");
            if (scroll)
                scroll->verticalScrollBar()->setValue(0);
            SettleLayouts();
            if (!renderDirectory.isEmpty())
            {
                expect(window.grab().save(
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
        expect(window.tabs->count() == 6, "main navigation must group related pages into six readable destinations");
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
                expect(window.tabs->tabText(window.tabs->currentIndex()) == "Companion" &&
                           window.findChild<QWidget*>("voiceTab")->isVisible(),
                    "Details must open Companion and its existing Voice view");
            }
        }
        const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
        if (!renderDirectory.isEmpty())
            expect(QDir().mkpath(renderDirectory), "render directory must be writable");
        QList<QWidget*> pages;
        for (auto* group : window.findChildren<QTabWidget*>())
        {
            for (int index = 0; index < group->count(); ++index)
            {
                QWidget* page = group->widget(index);
                if (qobject_cast<QTabWidget*>(page) == nullptr && page->findChild<QTabWidget*>() == nullptr)
                {
                    page->setProperty("captureTitle", group->tabText(index));
                    pages.push_back(page);
                }
            }
        }
        expect(pages.size() == 21, "all existing pages, including Mind's four views, must remain reachable");
        for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
        {
            window.resize(size);
            SettleLayouts();
            expect(window.size() == size, QString("the full interface must fit %1x%2").arg(size.width()).arg(size.height()));
            for (auto* group : window.findChildren<QTabWidget*>())
            {
                auto* bar = group->tabBar();
                expect(!bar->usesScrollButtons(), "navigation must not depend on left/right scroll arrows");
            }
            for (QWidget* page : pages)
            {
                revia::desktop::SelectNavigationPage(page);
                SettleLayouts();
                expect(page->isVisible(), "each subpage must activate its full navigation route");
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
                                             .toLower();
                    const QString path =
                        QDir(renderDirectory).filePath(QString("%1-%2x%3.png").arg(name).arg(size.width()).arg(size.height()));
                    expect(window.grab().save(path), "actual Qt render must be saved");
                }
            }
        }
        window.tabs->setCurrentIndex(0);
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
            window.tabs->setCurrentIndex(0);
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
                revia::desktop::SelectNavigationPage(window.findChild<QWidget*>("memoryTab"));
                for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
                {
                    window.resize(size);
                    SettleLayouts();
                    expect(window.size() == size, "memory revision provenance must fit the requested window size");
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
                    expect(window.grab().save(
                               QDir(renderDirectory).filePath(QString("memory-revision-%1x%2.png").arg(size.width()).arg(size.height()))),
                        "actual historical and current memory rows must have fresh Qt captures");
                }
            }
        }
        window.ConfigureMemoryRevisionControls();
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

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
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
    int failures = 0;
    ReviaWindow window(false, false);
    ReviaWindowStopTests::Run(window, failures);
    ReviaWindowStopTests::RunConfirmationAudienceUi(window, failures);
    ReviaWindowStopTests::RunConversationQualityUi(window, failures);
    ReviaWindowStopTests::RunMemoryRevisionUi(window, failures);
    ReviaWindowStopTests::RunHealthUiAndLayout(window, failures);
    ReviaWindowStopTests::RunCompanionStudio(window, failures);
    if (failures == 0)
        std::cout << "Desktop Stop policy regression passed.\n";
    return failures == 0 ? 0 : 1;
}
