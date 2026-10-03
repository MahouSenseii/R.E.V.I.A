#include "agentStudioPanel.h"
#include "studioDuration.h"

#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
QString Text(const std::string& value)
{
    return QString::fromStdString(value);
}

QString Duration(const std::uint64_t milliseconds)
{
    return revia::desktop::StudioDuration(milliseconds);
}

QString ArtifactDetails(const revia::agents::ArtifactReference& artifact)
{
    return Text(artifact.nodeId) + " · " + Text(artifact.id) + " · v" + QString::number(artifact.version) +
           "\nSHA256: " + Text(artifact.hash);
}
}

AgentStudioPanel::AgentStudioPanel(Controls inputControls, QWidget* parent) : QWidget(parent), controls(std::move(inputControls))
{
    setObjectName("agentStudioPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    auto* title = new QLabel("Agent Studio", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);
    auto* introduction = new QLabel("Give your companion a bounded objective. Two workers develop an approach and evidence; a reviewer "
                                    "checks their deliverables before companion acceptance.",
        this);
    introduction->setWordWrap(true);
    introduction->setToolTip("Provider durations include router/model waiting. Model queue timing is unavailable.");
    introduction->setObjectName("secondaryText");
    layout->addWidget(introduction);

    auto* request = new QGroupBox("Delegated work", this);
    auto* requestLayout = new QGridLayout(request);
    objective = new QPlainTextEdit(request);
    objective->setObjectName("agentObjective");
    objective->setPlaceholderText("Describe a question, plan or analytical deliverable…");
    objective->setMaximumHeight(90);
    requestLayout->addWidget(objective, 0, 0, 1, 3);
    provider = new QComboBox(request);
    provider->setObjectName("agentProvider");
    provider->addItem("Companion local model", false);
    provider->addItem("Deterministic diagnostic", true);
    requestLayout->addWidget(provider, 1, 0, 1, 3);
    start = new QPushButton("Start workflow", request);
    start->setObjectName("agentStart");
    cancel = new QPushButton("Cancel", request);
    auto* resume = new QPushButton("Resume saved", request);
    requestLayout->addWidget(start, 2, 0);
    requestLayout->addWidget(cancel, 2, 1);
    requestLayout->addWidget(resume, 2, 2);
    layout->addWidget(request);
    summary = new QLabel("No workflow started. Provider telemetry is unavailable.", this);
    summary->setObjectName("agentSummary");
    summary->setWordWrap(true);
    layout->addWidget(summary);
    hierarchy = new QTreeWidget(this);
    hierarchy->setObjectName("agentHierarchy");
    hierarchy->setHeaderLabels({"Level · role", "Working on", "State"});
    hierarchy->header()->setSectionResizeMode(QHeaderView::Stretch);
    hierarchy->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    hierarchy->setMinimumHeight(165);
    hierarchy->setMaximumHeight(260);
    layout->addWidget(hierarchy);
    details = new QLabel(this);
    details->setObjectName("agentDetails");
    details->setTextFormat(Qt::PlainText);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(details);
    auto* freshness = new QTimer(this);
    freshness->setInterval(250);
    connect(freshness, &QTimer::timeout, this, [this]() { ShowSelectedNode(); });
    freshness->start();
    deliverable = new QPlainTextEdit(this);
    deliverable->setObjectName("agentDeliverable");
    deliverable->setReadOnly(true);
    deliverable->setMaximumHeight(220);
    deliverable->setPlaceholderText("The accepted deliverable appears here after companion acceptance.");
    layout->addWidget(deliverable);
    auto* acceptance = new QGridLayout();
    accept = new QPushButton("Accept reviewed work", this);
    accept->setObjectName("agentAccept");
    auto* reject = new QPushButton("Reject", this);
    acceptance->addWidget(accept, 0, 0);
    acceptance->addWidget(reject, 0, 1);
    layout->addLayout(acceptance);

    auto* recovery = new QGroupBox("Recovery with changed evidence", this);
    auto* recoveryLayout = new QGridLayout(recovery);
    retryNode = new QComboBox(recovery);
    retryNode->setObjectName("agentRetryNode");
    recoveryLayout->addWidget(retryNode, 0, 0, 1, 2);
    revisedInput = new QPlainTextEdit(recovery);
    revisedInput->setObjectName("agentRevisedInput");
    revisedInput->setPlaceholderText("Revised task input");
    revisedInput->setMaximumHeight(65);
    evidence = new QPlainTextEdit(recovery);
    evidence->setObjectName("agentEvidence");
    evidence->setPlaceholderText("Changed evidence or repair reference");
    evidence->setMaximumHeight(65);
    recoveryLayout->addWidget(revisedInput, 1, 0, 1, 2);
    recoveryLayout->addWidget(evidence, 2, 0, 1, 2);
    auto* retry = new QPushButton("Retry within budget", recovery);
    retry->setObjectName("agentRetry");
    auto* fixtureRepair = new QPushButton("Use fixture repair", recovery);
    fixtureRepair->setObjectName("agentFixtureRepair");
    recoveryLayout->addWidget(retry, 3, 0);
    recoveryLayout->addWidget(fixtureRepair, 3, 1);
    layout->addWidget(recovery);
    feedback = new QLabel(this);
    feedback->setObjectName("agentFeedback");
    feedback->setTextFormat(Qt::PlainText);
    feedback->setWordWrap(true);
    layout->addWidget(feedback);
    layout->addStretch();

    connect(start, &QPushButton::clicked, this,
        [this]()
        {
            std::string error;
            const bool demo = provider->currentData().toBool();
            const std::string input = objective->toPlainText().toStdString();
            ShowResult(controls.start(input, demo, error), error);
        });
    connect(cancel, &QPushButton::clicked, this, [this]() { controls.cancel(); });
    connect(resume, &QPushButton::clicked, this,
        [this]()
        {
            std::string error;
            ShowResult(controls.resume(error), error);
        });
    connect(accept, &QPushButton::clicked, this,
        [this]()
        {
            std::string error;
            ShowResult(controls.decide(revia::agents::ParentDecision::Accept, error), error);
        });
    connect(reject, &QPushButton::clicked, this,
        [this]()
        {
            std::string error;
            ShowResult(controls.decide(revia::agents::ParentDecision::Reject, error), error);
        });
    connect(retry, &QPushButton::clicked, this,
        [this]()
        {
            std::string error;
            ShowResult(controls.retry(retryNode->currentData().toString().toStdString(), revisedInput->toPlainText().toStdString(),
                           evidence->toPlainText().toStdString(), error),
                error);
        });
    connect(fixtureRepair, &QPushButton::clicked, this,
        [this]()
        {
            if (!provider->currentData().toBool())
            {
                ShowResult(false, "Fixture repair is available for the deterministic diagnostic only.");
                return;
            }
            revisedInput->setPlainText("Repeat the diagnostic after replacing its deliberately incomplete fixture evidence.");
            evidence->setPlainText("fixture-evidence-repaired-v2");
            const int index = retryNode->findData("verification");
            if (index >= 0)
                retryNode->setCurrentIndex(index);
        });
    connect(hierarchy, &QTreeWidget::itemSelectionChanged, this, [this]() { ShowSelectedNode(); });
    SetSnapshot({});
}

void AgentStudioPanel::ShowResult(const bool success, const std::string& message)
{
    feedback->setText(message.empty() ? (success ? "Workflow request accepted." : "The request was refused.") : Text(message));
}

void AgentStudioPanel::ResetForCompanion()
{
    objective->clear();
    revisedInput->clear();
    evidence->clear();
    feedback->clear();
    provider->setCurrentIndex(0);
    SetSnapshot({});
}

void AgentStudioPanel::SetSnapshot(const revia::agents::WorkflowSnapshot& incoming)
{
    using namespace revia::agents;
    snapshot = incoming;
    observationAge.start();
    observedUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    deliverable->setVisible(snapshot.state == WorkflowState::Accepted);
    if (snapshot.state == WorkflowState::Accepted && controls.deliverable)
    {
        deliverable->setPlainText(Text(controls.deliverable()));
    }
    else
        deliverable->clear();
    const bool running = snapshot.state == WorkflowState::Running || snapshot.state == WorkflowState::Waiting;
    start->setEnabled(!running);
    cancel->setEnabled(running);
    accept->setEnabled(snapshot.state == WorkflowState::AwaitingAcceptance);
    if (snapshot.id.empty())
    {
        summary->setText("No workflow started. Provider telemetry is unavailable.");
        hierarchy->clear();
        details->clear();
        retryNode->clear();
        return;
    }
    summary->setText(Text(ToString(snapshot.state)) + " · parent: " + Text(ToString(snapshot.parentDecision)) +
                     " · requests: " + QString::number(snapshot.requests) + " · reported tokens: " +
                     (snapshot.unreportedRequests || running ? QString::number(snapshot.reportedTokens) + " + unavailable usage"
                                                             : QString::number(snapshot.reportedTokens)));
    QString selectedId;
    if (auto* selected = hierarchy->currentItem())
        selectedId = selected->data(0, Qt::UserRole).toString();
    QSignalBlocker block(hierarchy);
    QSignalBlocker retryBlock(retryNode);
    hierarchy->clear();
    const QString retryId = retryNode->currentData().toString();
    retryNode->clear();
    QTreeWidgetItem* parent = nullptr;
    for (const auto& node : snapshot.nodes)
    {
        if (node.role == WorkflowRole::Parent)
        {
            parent = new QTreeWidgetItem(hierarchy,
                {"L" + QString::number(node.level) + " · " + Text(ToString(node.role)), Text(node.workingOn), Text(ToString(node.state))});
            parent->setData(0, Qt::UserRole, Text(node.id));
            if (selectedId == Text(node.id))
                hierarchy->setCurrentItem(parent);
        }
    }
    for (const auto& node : snapshot.nodes)
    {
        if (node.role == WorkflowRole::Parent)
            continue;
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(hierarchy);
        item->setText(0, "L" + QString::number(node.level) + " · " + Text(ToString(node.role)));
        item->setText(1, Text(node.workingOn));
        item->setText(2, Text(ToString(node.state)));
        item->setData(0, Qt::UserRole, Text(node.id));
        if (selectedId == Text(node.id))
            hierarchy->setCurrentItem(item);
        if (node.state == WorkflowState::Failed || node.state == WorkflowState::Interrupted)
        {
            retryNode->addItem(Text(node.workingOn), Text(node.id));
        }
    }
    hierarchy->expandAll();
    if (!hierarchy->currentItem() && parent)
        hierarchy->setCurrentItem(parent);
    const int retryIndex = retryNode->findData(retryId);
    if (retryIndex >= 0)
        retryNode->setCurrentIndex(retryIndex);
    ShowSelectedNode();
}

void AgentStudioPanel::ShowSelectedNode()
{
    auto* item = hierarchy->currentItem();
    if (!item)
        return;
    const std::string id = item->data(0, Qt::UserRole).toString().toStdString();
    const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [&](const auto& node) { return node.id == id; });
    if (found == snapshot.nodes.end())
        return;
    const bool stale = observationAge.isValid() && observationAge.elapsed() > 2000;
    QString text = "Level " + QString::number(found->level) + " · " + Text(revia::agents::ToString(found->role)) + " · " +
                   Text(revia::agents::ToString(found->state)) + "\nParent: " + (found->parentId.empty() ? "None" : Text(found->parentId)) +
                   "\nSnapshot revision: " + QString::number(snapshot.sequence) + "\nLast observed UTC: " + observedUtc +
                   (stale ? " · Snapshot stale; refresh needed" : " · current observation") + "\nWorking on: " + Text(found->workingOn) +
                   "\nCurrent action: " + Text(found->currentAction) + "\nWaiting for: " + Text(found->waitingFor) +
                   "\nNext handoff: " + Text(found->nextHandoff) + "\nProvider interval " + Duration(found->activeMilliseconds) +
                   " · waiting " + Duration(found->waitingMilliseconds) + " · paused " + Duration(found->pausedMilliseconds) +
                   "\nAttempts: " + QString::number(found->attempts.size());
    QStringList dependencies;
    for (const auto& dependency : found->dependencies)
        dependencies.push_back(Text(dependency));
    text += "\nDependencies: " + (dependencies.empty() ? "None" : dependencies.join(", "));
    text += "\nRecorded node intervals: " + Duration(found->activeMilliseconds + found->waitingMilliseconds + found->pausedMilliseconds);
    if (!found->attempts.empty())
    {
        const auto& attempt = found->attempts.back();
        text += " · execution " + QString(attempt.executionSucceeded ? "succeeded" : "incomplete") + " · evidence " +
                QString(attempt.verified ? "verified" : "unverified") +
                "\nModel: " + (attempt.reportedModel ? Text(*attempt.reportedModel) : "unavailable") +
                " · tokens: " + (attempt.reportedTokens ? QString::number(*attempt.reportedTokens) : "unavailable");
    }
    for (const auto& attempt : found->attempts)
    {
        text += "\nAttempt " + QString::number(attempt.ordinal) + ": " + Text(revia::agents::ToString(attempt.state)) + " · process " +
                QString(attempt.executionSucceeded ? "succeeded" : "incomplete") + " · evidence " +
                QString(attempt.verified ? "verified" : "unverified") + "\nOutcome: " + Text(attempt.diagnostic);
        for (const auto& prerequisite : attempt.prerequisites)
            text += "\nUsed verified artifact: " + ArtifactDetails(prerequisite);
        if (attempt.artifact)
            text += "\nVerified deliverable: " + ArtifactDetails(*attempt.artifact);
    }
    details->setText(text);
}
