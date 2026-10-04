#include "reviaWindow.h"
#include "Actions/actionTypes.h"

#include "agentStudioPanel.h"
#include "learningStudioPanel.h"
#include "developmentStudioPanel.h"
#include "audienceStudioPanel.h"
#include "capabilityPanel.h"
#include "canvasPanel.h"
#include "internetActivityPanel.h"
#include "memoryPanel.h"
#include "mindPanel.h"
#include "pipelinePanel.h"
#include "profilePanel.h"
#include "resourcePanel.h"
#include "visionPanel.h"
#include "voiceHealthPanel.h"
#include "ui_reviaWindow.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <stdexcept>

revia::runtime::ReviaSession& ReviaWindow::Session()
{
    return selectedSession ? *selectedSession : session;
}

const revia::runtime::ReviaSession& ReviaWindow::Session() const
{
    return selectedSession ? *selectedSession : session;
}

void ReviaWindow::BindSession()
{
    const auto epoch = sessionUiEpoch.load();
    subscriptionId = Session().Events().Subscribe(
        [this, epoch](const revia::runtime::RuntimeEvent& event)
        {
            QMetaObject::invokeMethod(
                this,
                [this, epoch, event]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    if (!event.stamp.companionId.empty() && !event.stamp.SameSession(Session().Stamp()))
                        return;
                    HandleRuntimeEvent(event);
                },
                Qt::QueuedConnection);
        });
    Session().SetConfirmationHandler([this](const revia::actions::ActionRequest& request, const revia::actions::PolicyDecision& decision)
        { return ConfirmAction(request, decision); });
    Session().SetDesktopApprovalHandler([this](const revia::policy::ApprovalPrompt& prompt) { return ApproveDesktopEffect(prompt); });
}

void ReviaWindow::BuildCompanionControls()
{
    if (!companions)
        companions = std::make_unique<revia::runtime::CompanionRegistry>(Session().Paths().InstallRoot());
    std::string error;
    const bool loaded = companions->Load(error);
    auto* group = new QGroupBox("Companions", ui->profilesPage);
    group->setObjectName("companionControls");
    auto* layout = new QGridLayout(group);
    companionCombo = new QComboBox(group);
    companionCombo->setObjectName("companionSelection");
    companionCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    companionCombo->setMinimumContentsLength(12);
    createCompanionButton = new QPushButton("New companion", group);
    companionStatus = new QLabel(group);
    companionStatus->setObjectName("companionStatus");
    companionStatus->setTextFormat(Qt::PlainText);
    companionStatus->setWordWrap(true);
    layout->addWidget(companionCombo, 0, 0);
    layout->addWidget(createCompanionButton, 0, 1);
    layout->addWidget(companionStatus, 1, 0, 1, 2);
    ui->profilesHostLayout->insertWidget(0, group);
    RefreshCompanions();
    companionCombo->setEnabled(loaded);
    createCompanionButton->setEnabled(loaded);
    if (!loaded)
        companionStatus->setText(QString::fromStdString(error));
    connect(companionCombo, QOverload<int>::of(&QComboBox::activated), this,
        [this](const int index) { SwitchCompanion(companionCombo->itemData(index).toString().toStdString()); });
    connect(createCompanionButton, &QPushButton::clicked, this, [this]() { CreateCompanion(); });

    auto* scroll = new QScrollArea(tabs);
    scroll->setObjectName("agentStudioScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    AgentStudioPanel::Controls controls;
    controls.start = [this](const std::string& objective, const bool demo, std::string& error)
    {
        if (switchingCompanion)
        {
            error = "Companion selection is in progress.";
            return false;
        }
        return Session().StartAgentWorkflow(objective, demo, error);
    };
    controls.cancel = [this]() { Session().CancelAgentWorkflow(); };
    controls.resume = [this](std::string& error) { return Session().ResumeAgentWorkflow(error); };
    controls.retry = [this](const std::string& id, const std::string& input, const std::string& evidence, std::string& error)
    { return Session().RetryAgentNode(id, input, evidence, error); };
    controls.decide = [this](const revia::agents::ParentDecision decision, std::string& error)
    { return Session().DecideAgentWorkflow(decision, error); };
    controls.deliverable = [this]() { return Session().AgentWorkflowResult(); };
    agentStudioPanel = new AgentStudioPanel(std::move(controls), scroll);
    scroll->setWidget(agentStudioPanel);
    tabs->addTab(scroll, "Agent Studio");
    BuildAdditionalStudioPanels();
}

void ReviaWindow::RunStudioOperation(std::function<bool(revia::runtime::ReviaSession&, std::stop_token, std::string&)> operation,
    std::function<void(bool, const std::string&)> completed)
{
    if (switchingCompanion || shuttingDown.load() || studioOperationRunning.exchange(true))
        return;
    if (studioWorker.joinable())
        studioWorker.join();
    auto* origin = &Session();
    const auto stamp = origin->Stamp();
    const auto epoch = sessionUiEpoch.load();
    learningStudioPanel->SetBusy(true);
    developmentStudioPanel->SetBusy(true);
    audienceStudioPanel->SetBusy(true);
    stopButton->setEnabled(true);
    studioWorker = std::jthread(
        [this, origin, stamp, epoch, operation = std::move(operation), completed = std::move(completed)](const std::stop_token stop)
        {
            bool success = false;
            std::string message;
            try
            {
                if (!stop.stop_requested() && origin->Admits(stamp))
                    success = operation(*origin, stop, message);
                else
                    message = "This companion operation is no longer current.";
            }
            catch (...)
            {
                message = "The studio operation did not complete. Its stored state remains available for inspection.";
            }
            QMetaObject::invokeMethod(
                this,
                [this, origin, stamp, epoch, success, message, completed]
                {
                    if (shuttingDown.load() || epoch != sessionUiEpoch.load())
                        return;
                    studioOperationRunning.store(false);
                    learningStudioPanel->SetBusy(false);
                    developmentStudioPanel->SetBusy(false);
                    audienceStudioPanel->SetBusy(false);
                    if (&Session() != origin || !origin->Admits(stamp))
                        return;
                    completed(success, message);
                    RefreshStudioPanels();
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::BuildAdditionalStudioPanels()
{
    const auto add = [this](QWidget* panel, const char* objectName, const char* title)
    {
        auto* scroll = new QScrollArea(tabs);
        scroll->setObjectName(objectName);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(panel);
        tabs->addTab(scroll, title);
    };
    LearningStudioPanel::Controls learning;
    learning.inventory = [this](const std::string& directory)
    {
        RunStudioOperation(
            [directory](auto& session, const auto stop, std::string& message)
            {
                const auto result = session.RunInventorySkill(revia::actions::Utf8ToPath(directory), stop);
                message = result.text;
                return result.succeeded;
            },
            [this](const bool success, const auto& message) { learningStudioPanel->SetOutcome(success, message); });
    };
    learning.update = [this]
    {
        RunStudioOperation([](auto& session, auto, std::string& message) { return session.UpdateInventorySkill(message); },
            [this](const bool success, const auto& message) { learningStudioPanel->SetOutcome(success, message); });
    };
    learning.rollback = [this]
    {
        RunStudioOperation([](auto& session, auto, std::string& message) { return session.RollbackInventorySkill(message); },
            [this](const bool success, const auto& message) { learningStudioPanel->SetOutcome(success, message); });
    };
    learning.exportPackage = [this]
    {
        RunStudioOperation(
            [](auto& session, auto, std::string& message)
            {
                std::filesystem::path directory;
                if (!session.ExportInventorySkill(directory, message))
                    return false;
                message = "Reviewed neutral assets exported to " + revia::actions::PathToUtf8(directory);
                return true;
            },
            [this](const bool success, const auto& message) { learningStudioPanel->SetOutcome(success, message); });
    };
    learning.decide = [this](const std::string& id, const auto decision, const std::string& feedback)
    {
        RunStudioOperation([id, decision, feedback](auto& session, auto, std::string& message)
            { return session.ReviewLearning(id, decision, feedback, message); },
            [this](const bool success, const auto& message) { learningStudioPanel->SetOutcome(success, message); });
    };
    learningStudioPanel = new LearningStudioPanel(std::move(learning));
    add(learningStudioPanel, "learningStudioScroll", "Skills && Learning");
    DevelopmentStudioPanel::Controls development;
    development.propose = [this]
    {
        RunStudioOperation(
            [](auto& session, const auto stop, std::string& message)
            {
                std::string id;
                if (!session.ProposePresentationChange(id, message, stop))
                    return false;
                message = "Presentation proposal recorded: " + id;
                return true;
            },
            [this](const bool success, const auto& message) { developmentStudioPanel->SetOutcome(success, message); });
    };
    development.validate = [this]
    {
        RunStudioOperation([](auto& session, const auto stop, std::string& message)
            { return session.ReviewPresentationChange(true, message, stop); },
            [this](const bool success, const auto& message) { developmentStudioPanel->SetOutcome(success, message); });
    };
    development.review = [this]
    {
        RunStudioOperation([](auto& session, const auto stop, std::string& message)
            { return session.ReviewPresentationChange(false, message, stop); },
            [this](const bool success, const auto& message) { developmentStudioPanel->SetOutcome(success, message); });
    };
    developmentStudioPanel = new DevelopmentStudioPanel(std::move(development));
    add(developmentStudioPanel, "developmentStudioScroll", "Self Development");
    AudienceStudioPanel::Controls audience;
    audience.audience = [this](const auto context)
    {
        RunStudioOperation([context](auto& session, auto, std::string& message) { return session.SetAudience(context, message); },
            [this](const bool success, const auto& message) { audienceStudioPanel->SetOutcome(success, message); });
    };
    audience.enroll = [this](const std::string& entity, const std::string& wave, const bool consent)
    {
        RunStudioOperation([entity, wave, consent](auto& session, auto, std::string& message)
            { return session.EnrollSpeaker(entity, revia::actions::Utf8ToPath(wave), consent, message); },
            [this](const bool success, const auto& message) { audienceStudioPanel->SetOutcome(success, message); });
    };
    audience.forget = [this](const std::string& entity)
    {
        RunStudioOperation([entity](auto& session, auto, std::string& message) { return session.ForgetSpeaker(entity, message); },
            [this](const bool success, const auto& message) { audienceStudioPanel->SetOutcome(success, message); });
    };
    audience.alias = [this](const std::string& entity, const std::string& alias)
    {
        RunStudioOperation([entity, alias](auto& session, auto, std::string& message)
            { return session.SetAudienceAlias(entity, alias, message); },
            [this](const bool success, const auto& message) { audienceStudioPanel->SetOutcome(success, message); });
    };
    audience.correct = [this](const std::string& evidence, const std::string& entity)
    {
        RunStudioOperation([evidence, entity](auto& session, auto, std::string& message)
            { return session.CorrectSpeakerEvidence(evidence, entity, message); },
            [this](const bool success, const auto& message) { audienceStudioPanel->SetOutcome(success, message); });
    };
    audienceStudioPanel = new AudienceStudioPanel(std::move(audience));
    add(audienceStudioPanel, "audienceStudioScroll", "Audience && Recognition");
    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] { RefreshStudioPanels(); });
    timer->start();
}

void ReviaWindow::RefreshStudioPanels()
{
    if (switchingCompanion || shuttingDown.load() || studioOperationRunning.load())
        return;
    learningStudioPanel->SetSnapshot(Session().LearningStudio());
    developmentStudioPanel->SetSnapshot(Session().DevelopmentStudio());
    audienceStudioPanel->SetSnapshot(Session().Audience(), Session().Relationships(), Session().RelationshipEvidence());
}

void ReviaWindow::RefreshCompanions()
{
    QSignalBlocker block(companionCombo);
    companionCombo->clear();
    for (const auto& companion : companions->List())
    {
        companionCombo->addItem(QString::fromStdString(companion.displayName) + (companion.legacy ? " · original mind" : ""),
            QString::fromStdString(companion.id));
    }
    const int active = companionCombo->findData(QString::fromStdString(Session().Paths().Descriptor().id));
    if (active >= 0)
        companionCombo->setCurrentIndex(active);
    companionStatus->setText("Each companion keeps its own memory, development, relationships, histories and voice data. Profiles edit the "
                             "selected companion's authored settings.");
}

void ReviaWindow::CreateCompanion()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, "New companion", "Companion name", QLineEdit::Normal, {}, &accepted);
    if (!accepted || name.trimmed().isEmpty())
        return;
    revia::runtime::CompanionDescriptor created;
    std::string error;
    if (!companions->Create(name.trimmed().toStdString(), "assistant", created, error))
    {
        companionStatus->setText(QString::fromStdString(error));
        return;
    }
    RefreshCompanions();
    SwitchCompanion(created.id);
}

void ReviaWindow::SwitchCompanion(const std::string& id)
{
    if (switchingCompanion || shuttingDown.load() || id == Session().Paths().Descriptor().id)
        return;
    const auto destination = companions->Find(id);
    if (!destination)
        return;
    if (companionSwitchWorker.joinable())
        companionSwitchWorker.join();
    switchingCompanion = true;
    ResetConversationPresentation();
    const bool restart = Session().IsStarted();
    const auto epoch = sessionUiEpoch.fetch_add(1) + 1;
    Session().Events().Unsubscribe(subscriptionId);
    subscriptionId = 0;
    AbandonQuestions();
    Session().RequestStop();
    studioWorker.request_stop();
    companionCombo->setEnabled(false);
    createCompanionButton->setEnabled(false);
    agentStudioPanel->setEnabled(false);
    tabs->setEnabled(false);
    sendButton->setEnabled(false);
    companionStatus->setText("Switching companion: stopping outgoing work and releasing model and device ownership…");
    auto* outgoing = &Session();
    companionSwitchWorker = std::jthread(
        [this, outgoing, destination = *destination, restart, epoch]()
        {
            if (operationWorker.joinable())
                operationWorker.join();
            if (capabilityWorker.joinable())
                capabilityWorker.join();
            if (deviceWorker.joinable())
                deviceWorker.join();
            if (voiceWorker.joinable())
                voiceWorker.join();
            if (studioWorker.joinable())
                studioWorker.join();
            outgoing->Stop();
            QMetaObject::invokeMethod(
                this,
                [this, destination, restart, epoch]()
                {
                    if (shuttingDown.load() || epoch != sessionUiEpoch.load())
                        return;
                    std::string error;
                    try
                    {
                        if (initializedCompanionAuthority.insert(destination.id).second)
                            session.Authority()->SetCompanionDefaults(
                                destination.id, revia::policy::AuthorityPermissions::WithinMachineCeiling());
                        auto incoming = std::make_unique<revia::runtime::ReviaSession>(
                            revia::runtime::CompanionPaths(Session().Paths().InstallRoot(), destination), session.Authority());
                        if (!companions->Select(destination.id, error))
                            throw std::runtime_error(error);
                        auto retiring = std::move(selectedSession);
                        selectedSession = std::move(incoming);
                        chatEntries.clear();
                        ResetConversationPresentation();
                        activityEntries.clear();
                        lastComponentIssues.clear();
                        activityWarningCount = 0;
                        activityErrorCount = 0;
                        RenderChat();
                        RenderActivity();
                        messageInput->clear();
                        pendingUtterances.clear();
                        pendingSpeechTimer->stop();
                        speechPhase.clear();
                        agentStudioPanel->ResetForCompanion();
                        learningStudioPanel->ResetForCompanion();
                        developmentStudioPanel->ResetForCompanion();
                        audienceStudioPanel->ResetForCompanion();
                        studioOperationRunning.store(false);
                        listenRequested = false;
                        voiceOperationRunning.store(false);
                        voiceNameInput->clear();
                        voiceDescriptionInput->clear();
                        voiceReferenceInput->clear();
                        voicePreviewInput->clear();
                        voiceLibraryCombo->clear();
                        microphoneTestResultLabel->clear();
                        for (QPushButton* button : {createVoiceButton, previewVoiceButton, renderVoiceBankButton, testMicrophoneButton})
                            button->setEnabled(true);
                        ApplyMicrophoneUi(MicrophoneUi::Unavailable);
                        RebuildSessionPanels();
                        RefreshVoiceStudio();
                        retiring.reset();
                        questions.Reopen();
                        BindSession();
                        RefreshCompanions();
                        ApplyUserPreferences();
                        UpdateState(revia::runtime::RuntimeState::Offline, "Selected " + QString::fromStdString(destination.displayName));
                    }
                    catch (const std::exception& failure)
                    {
                        companionStatus->setText(QString::fromUtf8(failure.what()));
                        questions.Reopen();
                        BindSession();
                    }
                    switchingCompanion = false;
                    companionCombo->setEnabled(true);
                    createCompanionButton->setEnabled(true);
                    agentStudioPanel->setEnabled(true);
                    tabs->setEnabled(true);
                    agentStudioPanel->SetSnapshot(Session().AgentWorkflowSnapshot());
                    RefreshStudioPanels();
                    if (restart)
                        StartRuntime();
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::RebuildSessionPanels()
{
    delete profilePanel;
    profilePanel = new ProfilePanel(Session(), ui->profilesPage);
    ui->profilesHostLayout->addWidget(profilePanel);
    delete memoryPanel;
    memoryPanel = new MemoryPanel(Session(), ui->memoryTab);
    ui->memoryHostLayout->addWidget(memoryPanel);
    ConfigureMemoryRevisionControls();
    delete mindPanel;
    mindPanel = new MindPanel(Session(), ui->mindTab);
    ui->mindHostLayout->addWidget(mindPanel);
    delete visionPanel;
    visionPanel = new VisionPanel(Session(), ui->visionTab);
    ui->visionHostLayout->addWidget(visionPanel);
    auto* permissions = qobject_cast<QScrollArea*>(ui->permissionsHostLayout->itemAt(0)->widget());
    delete permissions->takeWidget();
    capabilityPanel = new CapabilityPanel(Session(), [this]() { DiscoverApplicationPermissions(); }, permissions);
    permissions->setWidget(capabilityPanel);
    delete pipelinePanel;
    pipelinePanel = new PipelinePanel(ui->pipelinePage);
    ui->pipelineHostLayout->addWidget(pipelinePanel);
    delete internetActivityPanel;
    internetActivityPanel = new InternetActivityPanel(ui->internetActivityPage);
    ui->internetActivityHostLayout->addWidget(internetActivityPanel);
    delete canvasPanel;
    canvasPanel = new CanvasPanel(ui->canvasPage);
    ui->canvasHostLayout->addWidget(canvasPanel);
    delete resourcePanel;
    resourcePanel = new ResourcePanel(
        [this](const std::string& device) { return Session().SetPreference("resources.voiceDevice", device); }, ui->resourcePage);
    ui->resourceHostLayout->addWidget(resourcePanel);
    delete chatVoiceHealth;
    chatVoiceHealth = new VoiceHealthPanel(true, ui->chatPage);
    ui->chatLayout->insertWidget(1, chatVoiceHealth);
    delete voiceHealth;
    voiceHealth = new VoiceHealthPanel(false, ui->voicePage);
    ui->voiceLayout->insertWidget(0, voiceHealth);
    connect(chatVoiceHealth->DetailsButton(), &QPushButton::clicked, this, [this]() { tabs->setCurrentWidget(ui->voiceTab); });
}
