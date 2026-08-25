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
    refreshing = false;
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
