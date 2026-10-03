#include "reviaWindow.h"
#include "agentStudioPanel.h"
#include "audienceStudioPanel.h"
#include "developmentStudioPanel.h"
#include "learningStudioPanel.h"
#include "Memory/longTermMemory.h"
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
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
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
        for (int index = 0; index < window.tabs->count(); ++index)
        {
            if (window.tabs->tabText(index) == "Agent Studio")
                window.tabs->setCurrentIndex(index);
        }
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
        window.Session().Events().Publish(
            RuntimeEvent{RuntimeEventKind::AssistantMessage, RuntimeState::Idle, "late-original-ui-sentinel"});
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
        for (int index = 0; index < window.tabs->count(); ++index)
        {
            if (window.tabs->tabText(index) == "Agent Studio")
                window.tabs->setCurrentIndex(index);
        }
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
                expect(window.tabs->tabText(window.tabs->currentIndex()) == "Voice", "Details must open the existing Voice view");
            }
        }
        const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
        if (!renderDirectory.isEmpty())
            expect(QDir().mkpath(renderDirectory), "render directory must be writable");
        for (const auto size : {QSize(760, 540), QSize(1040, 720), QSize(1600, 1000)})
        {
            window.resize(size);
            SettleLayouts();
            expect(window.size() == size, QString("the full interface must fit %1x%2").arg(size.width()).arg(size.height()));
            for (int index = 0; index < window.tabs->count(); ++index)
            {
                window.tabs->setCurrentIndex(index);
                SettleLayouts();
                for (auto* scroll : window.tabs->widget(index)->findChildren<QScrollArea*>())
                {
                    if (!scroll->isVisible())
                        continue;
                    expect(
                        scroll->horizontalScrollBar()->maximum() == 0, QString("%1 at %2x%3 must fit horizontally without hidden overflow")
                                                                           .arg(window.tabs->tabText(index))
                                                                           .arg(size.width())
                                                                           .arg(size.height()));
                }
                if (!renderDirectory.isEmpty())
                {
                    const QString name = window.tabs->tabText(index).toLower().replace(' ', '_');
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
    ReviaWindowStopTests::RunHealthUiAndLayout(window, failures);
    ReviaWindowStopTests::RunCompanionStudio(window, failures);
    if (failures == 0)
        std::cout << "Desktop Stop policy regression passed.\n";
    return failures == 0 ? 0 : 1;
}
