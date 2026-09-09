#include "capabilityPanel.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

CapabilityPanel::CapabilityPanel(
    revia::runtime::ReviaSession& inputSession,
    DiscoveryRequest inputDiscoveryRequest,
    QWidget* parent)
    : QWidget(parent),
      session(inputSession),
      requestDiscovery(std::move(inputDiscoveryRequest))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* title = new QLabel("Application and internet permissions", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);
    auto* explanation = new QLabel(
        "Applications begin with read-only inspection permission and no mutable controls. "
        "Discover a foreground window, then approve only the controls Revia may invoke or edit. "
        "Internet access is a separate read-only, rate-limited capability.", this);
    explanation->setWordWrap(true);
    explanation->setObjectName("secondaryText");
    layout->addWidget(explanation);

    internetCheck = new QCheckBox("Allow bounded internet lookup", this);
    automaticLookupCheck = new QCheckBox(
        "Automatically look up current and factual knowledge questions", this);
    visibleBrowserCheck = new QCheckBox(
        "Use a dedicated visible browser window", this);
    autonomousResearchCheck = new QCheckBox(
        "Allow Revia to research her own topics", this);
    auto* internetRow = new QHBoxLayout();
    internetRow->addWidget(internetCheck);
    internetRow->addWidget(automaticLookupCheck);
    internetRow->addStretch();
    layout->addLayout(internetRow);
    auto* browserRow = new QHBoxLayout();
    browserRow->addWidget(visibleBrowserCheck);
    browserRow->addWidget(autonomousResearchCheck);
    browserRow->addStretch();
    layout->addLayout(browserRow);

    cameraCheck = new QCheckBox("Allow camera access", this);
    cameraCheck->setToolTip(
        "Revia may take a single still frame when you ask her to look at something. "
        "The camera is opened for that frame and closed again immediately.");
    autonomousCameraCheck = new QCheckBox(
        "Let Revia decide when to look", this);
    autonomousCameraCheck->setToolTip(
        "A separate permission. Without it she can only use the camera when asked.");
    auto* cameraRow = new QHBoxLayout();
    cameraRow->addWidget(cameraCheck);
    cameraRow->addWidget(autonomousCameraCheck);
    cameraRow->addStretch();
    layout->addLayout(cameraRow);

    auto* desktopTitle = new QLabel("Pointer and keyboard control", this);
    desktopTitle->setObjectName("sectionTitle");
    layout->addWidget(desktopTitle);
    auto* desktopExplanation = new QLabel(
        "Synthesized input is indistinguishable from you typing, so it is confined to "
        "windows that belong to an approved application above and refuses to act if "
        "something else has focus. Hold ctrl+alt+shift, or press Stop, to halt it "
        "immediately.", this);
    desktopExplanation->setWordWrap(true);
    desktopExplanation->setObjectName("secondaryText");
    layout->addWidget(desktopExplanation);

    pointerCheck = new QCheckBox("Move and click the pointer", this);
    keyboardCheck = new QCheckBox("Type and press key chords", this);
    launchCheck = new QCheckBox("Start approved applications", this);
    rawCoordinateCheck = new QCheckBox("Allow raw coordinates", this);
    rawCoordinateCheck->setToolTip(
        "Without this she may only click an element the screen resolver re-verified. "
        "Either way the point must land inside the approved application's window.");
    autonomousDesktopCheck = new QCheckBox("Let Revia operate on her own", this);
    autonomousDesktopCheck->setToolTip(
        "A separate permission. Without it she may only do this as part of something "
        "you asked for.");
    desktopStopButton = new QPushButton("Stop desktop control", this);
    auto* desktopRow = new QHBoxLayout();
    desktopRow->addWidget(pointerCheck);
    desktopRow->addWidget(keyboardCheck);
    desktopRow->addWidget(launchCheck);
    desktopRow->addStretch();
    layout->addLayout(desktopRow);
    auto* desktopScopeRow = new QHBoxLayout();
    desktopScopeRow->addWidget(rawCoordinateCheck);
    desktopScopeRow->addWidget(autonomousDesktopCheck);
    desktopScopeRow->addStretch();
    desktopScopeRow->addWidget(desktopStopButton);
    layout->addLayout(desktopScopeRow);

    auto* body = new QHBoxLayout();
    auto* approvedColumn = new QVBoxLayout();
    auto* approvedTitle = new QLabel("Approved applications and controls", this);
    approvedTitle->setObjectName("sectionTitle");
    approvedColumn->addWidget(approvedTitle);
    approvedTree = new QTreeWidget(this);
    approvedTree->setHeaderLabels({"Permission", "Scope"});
    approvedTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    approvedTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    approvedTree->setAlternatingRowColors(true);
    approvedTree->setSelectionMode(QAbstractItemView::SingleSelection);
    approvedColumn->addWidget(approvedTree, 1);
    auto* approvedButtons = new QHBoxLayout();
    auto* addApplicationButton = new QPushButton("Add application", this);
    auto* removeButton = new QPushButton("Remove selected", this);
    removeButton->setObjectName("stopButton");
    approvedButtons->addWidget(addApplicationButton);
    approvedButtons->addWidget(removeButton);
    approvedColumn->addLayout(approvedButtons);
    body->addLayout(approvedColumn, 1);

    auto* discoveryColumn = new QVBoxLayout();
    auto* discoveryTitle = new QLabel("Foreground control discovery", this);
    discoveryTitle->setObjectName("sectionTitle");
    discoveryColumn->addWidget(discoveryTitle);
    discoveryLabel = new QLabel(
        "Minimize Revia and inspect the application underneath. Discovery changes no permission.",
        this);
    discoveryLabel->setWordWrap(true);
    discoveryLabel->setObjectName("secondaryText");
    discoveryColumn->addWidget(discoveryLabel);
    discoveredTable = new QTableWidget(0, 3, this);
    discoveredTable->setHorizontalHeaderLabels({"Control", "Automation ID", "Pattern"});
    discoveredTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    discoveredTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    discoveredTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    discoveredTable->verticalHeader()->setVisible(false);
    discoveredTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    discoveredTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    discoveredTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    discoveredTable->setAlternatingRowColors(true);
    discoveryColumn->addWidget(discoveredTable, 1);
    auto* discoveryButtons = new QHBoxLayout();
    auto* discoverButton = new QPushButton("Inspect foreground app", this);
    approveDiscoveredButton = new QPushButton("Approve selected controls", this);
    approveDiscoveredButton->setEnabled(false);
    discoveryButtons->addWidget(discoverButton);
    discoveryButtons->addWidget(approveDiscoveredButton);
    discoveryColumn->addLayout(discoveryButtons);
    body->addLayout(discoveryColumn, 1);
    layout->addLayout(body, 1);

    statusLabel = new QLabel(
        "Permissions persist in RuntimeData/Capabilities/capabilities.json.", this);
    statusLabel->setObjectName("secondaryText");
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    connect(internetCheck, &QCheckBox::toggled, this, [this]() { ApplyInternetSettings(); });
    connect(automaticLookupCheck, &QCheckBox::toggled, this, [this]() { ApplyInternetSettings(); });
    connect(visibleBrowserCheck, &QCheckBox::toggled,
        this, [this]() { ApplyBrowserSettings(); });
    connect(autonomousResearchCheck, &QCheckBox::toggled,
        this, [this]() { ApplyBrowserSettings(); });
    connect(cameraCheck, &QCheckBox::toggled, this, [this]() { ApplyCameraSettings(); });
    connect(autonomousCameraCheck, &QCheckBox::toggled,
        this, [this]() { ApplyCameraSettings(); });
    connect(pointerCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(keyboardCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(launchCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(rawCoordinateCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(autonomousDesktopCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(desktopStopButton, &QPushButton::clicked, this, [this]() { ToggleDesktopStop(); });
    connect(addApplicationButton, &QPushButton::clicked, this,
        [this]() { AddApplicationManually(); });
    connect(removeButton, &QPushButton::clicked, this,
        [this]() { RemoveSelectedPermission(); });
    connect(discoverButton, &QPushButton::clicked, this, [this]()
    {
        if (requestDiscovery) requestDiscovery();
    });
    connect(approveDiscoveredButton, &QPushButton::clicked, this,
        [this]() { ApproveSelectedDiscoveredControls(); });
    Refresh();
}

void CapabilityPanel::Refresh()
{
    refreshing = true;
    const revia::actions::CapabilitySettings settings = session.Capabilities();
    approvedTree->clear();
    for (const std::string& application : settings.approvedApplications)
    {
        auto* applicationItem = new QTreeWidgetItem(approvedTree);
        applicationItem->setText(0, QString::fromStdString(application));
        applicationItem->setText(1, "Application");
        applicationItem->setData(0, Qt::UserRole, QString::fromStdString(application));
        const auto controls = std::find_if(
            settings.approvedControls.begin(), settings.approvedControls.end(),
            [&](const auto& entry)
            {
                return QString::fromStdString(entry.first).compare(
                    QString::fromStdString(application), Qt::CaseInsensitive) == 0;
            });
        if (controls != settings.approvedControls.end())
        {
            for (const std::string& control : controls->second)
            {
                auto* controlItem = new QTreeWidgetItem(applicationItem);
                controlItem->setText(0, QString::fromStdString(control));
                controlItem->setText(1, control == "*" ? "All mutable controls" : "Control");
                controlItem->setData(0, Qt::UserRole, QString::fromStdString(application));
                controlItem->setData(1, Qt::UserRole, QString::fromStdString(control));
            }
        }
        applicationItem->setExpanded(true);
    }
    internetCheck->setChecked(settings.internet.enabled);
    automaticLookupCheck->setChecked(settings.internet.automaticLookup);
    visibleBrowserCheck->setChecked(settings.internet.visibleBrowser);
    autonomousResearchCheck->setChecked(settings.internet.autonomousResearch);
    automaticLookupCheck->setEnabled(settings.internet.enabled);
    visibleBrowserCheck->setEnabled(settings.internet.enabled);
    autonomousResearchCheck->setEnabled(
        settings.internet.enabled && settings.internet.visibleBrowser);
    cameraCheck->setChecked(settings.camera.enabled);
    autonomousCameraCheck->setChecked(settings.camera.autonomousCapture);
    autonomousCameraCheck->setEnabled(settings.camera.enabled);
    const auto& desktop = settings.desktopControl;
    pointerCheck->setChecked(desktop.pointer);
    keyboardCheck->setChecked(desktop.keyboard);
    launchCheck->setChecked(desktop.applicationLaunch);
    rawCoordinateCheck->setChecked(desktop.rawCoordinates);
    rawCoordinateCheck->setEnabled(desktop.pointer);
    autonomousDesktopCheck->setChecked(desktop.autonomous);
    autonomousDesktopCheck->setEnabled(desktop.AnyEnabled());
    // Kept meaningful even with every capability off: the stop has to stay reachable
    // while it is latched, or the only way back would be editing JSON.
    desktopStopButton->setText(session.DesktopControlStopped()
        ? "Resume desktop control" : "Stop desktop control");
    refreshing = false;
}

void CapabilityPanel::ApplyCameraSettings()
{
    if (refreshing) return;
    const auto previous = session.Capabilities().camera;
    // Confirmed on the way up only, and worded so the answer is informed rather than
    // reflexive. A camera toggle that flips silently is the one permission users would
    // most reasonably expect to be asked about.
    if (!previous.enabled && cameraCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Allow camera access",
            "Revia will be able to take a single still frame from your camera and describe "
            "what it contains. The camera is opened for that frame and closed again "
            "immediately, so the hardware light is on only while a frame is being taken. "
            "Frames are written to RuntimeData/Camera on this computer and are never "
            "uploaded. Seeing something does not let her act on it: anything she does as a "
            "result still goes through the ordinary permission and confirmation path. "
            "Enable camera access?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    if (!previous.autonomousCapture && autonomousCameraCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Let Revia decide when to look",
            "This is a separate permission from answering a question about what she can see. "
            "It allows Revia to take a frame without you asking, subject to the same rate "
            "limit and the same audit trail. Consenting to answer \"what am I holding?\" is "
            "not the same as consenting to be watched, which is why this is asked "
            "separately. Allow it?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    autonomousCameraCheck->setEnabled(cameraCheck->isChecked());
    const auto result = session.SetCameraAccess(
        cameraCheck->isChecked(), autonomousCameraCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyDesktopControlSettings()
{
    if (refreshing) return;
    const auto previous = session.Capabilities().desktopControl;
    // Confirmed on the way up only. This is the permission that lets Revia act as the
    // person at the keyboard, so the question is asked once and answered informed.
    if (!previous.AnyEnabled() &&
        (pointerCheck->isChecked() || keyboardCheck->isChecked() ||
            launchCheck->isChecked()))
    {
        const bool approved = QMessageBox::question(
            this,
            "Allow pointer and keyboard control",
            "Revia will be able to move the pointer, click, and type on this computer. "
            "Input is confined to windows belonging to the applications approved below, "
            "and she refuses to act when something else has focus, but inside those "
            "windows it is the same as you doing it. Every action is rate limited and "
            "written to the action audit log; typed text is recorded only by length. "
            "Hold ctrl+alt+shift at any time to stop it. Enable desktop control?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    if (!previous.autonomous && autonomousDesktopCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Let Revia operate the desktop on her own",
            "This is a separate permission from doing desktop work you asked for. It "
            "allows Revia to move the pointer, type, and start approved applications "
            "without a request, subject to the same limits and the same audit trail. "
            "Allow it?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    const auto result = session.SetDesktopControl(
        pointerCheck->isChecked(),
        keyboardCheck->isChecked(),
        launchCheck->isChecked(),
        rawCoordinateCheck->isChecked(),
        autonomousDesktopCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ToggleDesktopStop()
{
    if (session.DesktopControlStopped())
    {
        const auto result = session.ResumeDesktopControl();
        SetStatus(QString::fromStdString(result.message), !result.succeeded);
    }
    else
    {
        session.StopDesktopControl("stopped from the Permissions tab");
        SetStatus("Desktop control is stopped. Nothing further will be typed or clicked.");
    }
    Refresh();
}

void CapabilityPanel::ShowDiscovery(
    const revia::actions::windows::ApplicationControlInventory& inventory)
{
    discoveredTable->setRowCount(0);
    discoveredApplication.clear();
    if (!inventory.succeeded)
    {
        discoveryLabel->setText(QString::fromStdString(inventory.reason));
        approveDiscoveredButton->setEnabled(false);
        SetStatus(QString::fromStdString(inventory.reason), true);
        return;
    }
    discoveredApplication = inventory.application;
    discoveryLabel->setText(
        QString::fromStdString(inventory.application + " — " + inventory.windowTitle +
            ". Select only controls Revia should be allowed to change."));
    for (const auto& control : inventory.controls)
    {
        const int row = discoveredTable->rowCount();
        discoveredTable->insertRow(row);
        auto* name = new QTableWidgetItem(QString::fromStdString(
            control.name.empty() ? control.permissionKey : control.name));
        name->setData(Qt::UserRole, QString::fromStdString(control.permissionKey));
        discoveredTable->setItem(row, 0, name);
        discoveredTable->setItem(
            row, 1, new QTableWidgetItem(QString::fromStdString(control.automationId)));
        const QString pattern = control.supportsInvoke && control.supportsValue
            ? "Invoke + Value"
            : control.supportsInvoke ? "Invoke" : "Value";
        discoveredTable->setItem(row, 2, new QTableWidgetItem(pattern));
    }
    approveDiscoveredButton->setEnabled(!inventory.controls.empty());
    SetStatus(QString::fromStdString(inventory.reason));
}

void CapabilityPanel::SetStatus(const QString& text, const bool error)
{
    statusLabel->setText(text);
    statusLabel->setStyleSheet(error ? "color: #ff8da1;" : "color: #85d7c8;");
}

void CapabilityPanel::AddApplicationManually()
{
    bool accepted = false;
    const QString executable = QInputDialog::getText(
        this, "Approve application", "Executable name (for example, notepad.exe)",
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || executable.isEmpty()) return;
    const auto result = session.AddApprovedApplication(executable.toStdString());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApproveSelectedDiscoveredControls()
{
    if (discoveredApplication.empty()) return;
    revia::runtime::CapabilityUpdateResult result =
        session.AddApprovedApplication(discoveredApplication);
    if (!result.succeeded)
    {
        SetStatus(QString::fromStdString(result.message), true);
        return;
    }
    const QModelIndexList rows = discoveredTable->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows)
    {
        const QString key = discoveredTable->item(index.row(), 0)->data(Qt::UserRole).toString();
        result = session.AddApprovedControl(discoveredApplication, key.toStdString());
        if (!result.succeeded) break;
    }
    SetStatus(QString::fromStdString(
        rows.empty()
            ? "Application approved for inspection only; no mutable controls were selected."
            : result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::RemoveSelectedPermission()
{
    QTreeWidgetItem* item = approvedTree->currentItem();
    if (item == nullptr) return;
    const QString application = item->data(0, Qt::UserRole).toString();
    const QString control = item->data(1, Qt::UserRole).toString();
    const QString description = control.isEmpty()
        ? QStringLiteral("all permissions for %1").arg(application)
        : QStringLiteral("control '%1' from %2").arg(control, application);
    if (QMessageBox::question(
            this, "Remove permission", "Remove " + description + "?") != QMessageBox::Yes)
    {
        return;
    }
    const auto result = control.isEmpty()
        ? session.RemoveApprovedApplication(application.toStdString())
        : session.RemoveApprovedControl(application.toStdString(), control.toStdString());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyInternetSettings()
{
    if (refreshing) return;
    const bool wasEnabled = session.Capabilities().internet.enabled;
    if (!wasEnabled && internetCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Enable internet lookup",
            "Knowledge questions may be sent to the configured DuckDuckGo and Wikipedia "
            "HTTPS endpoints or, when selected below, to Revia's dedicated visible browser. "
            "Every lookup is read-only, bounded, rate limited, and audited. Enable this "
            "capability?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            refreshing = true;
            internetCheck->setChecked(false);
            refreshing = false;
            return;
        }
    }
    automaticLookupCheck->setEnabled(internetCheck->isChecked());
    visibleBrowserCheck->setEnabled(internetCheck->isChecked());
    autonomousResearchCheck->setEnabled(
        internetCheck->isChecked() && visibleBrowserCheck->isChecked());
    const auto result = session.SetInternetAccess(
        internetCheck->isChecked(), automaticLookupCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyBrowserSettings()
{
    if (refreshing) return;
    const auto previous = session.Capabilities().internet;
    if (!previous.visibleBrowser && visibleBrowserCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Enable visible browsing",
            "Revia will open a separate, visible Edge or Chrome window using a dedicated "
            "RuntimeData browser profile. She may search public HTTPS pages and read bounded "
            "page text, but cannot use your personal cookies, downloads, uploads, passwords, "
            "payments, localhost, or private-network pages. Queries, pages, results, timing, "
            "and failures remain visible and audited. Enable it?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    if (!previous.autonomousResearch && autonomousResearchCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Allow autonomous research",
            "This allows Revia to choose a read-only web query without a new message from "
            "you when recent conversation or a meaningful emotion creates a concrete topic. "
            "A timer alone cannot create a topic, and attention, cooldown, deduplication, "
            "hourly, browser, and network limits still apply. Allow this separate permission?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }

    const auto result = session.SetInternetBrowser(
        visibleBrowserCheck->isChecked(),
        autonomousResearchCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}
