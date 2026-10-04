#include "reviaWindow.h"
#include "ui_reviaWindow.h"
#include "navigationSidebar.h"
#include "tabNavigation.h"

#include <QBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>

#include <algorithm>
#include <vector>

namespace
{
void ReflowGrid(QGridLayout* layout, const int columns)
{
    if (layout->property("columns").toInt() == columns)
        return;
    layout->setProperty("columns", columns);
    std::vector<QLayoutItem*> items;
    while (auto* item = layout->takeAt(0))
        items.push_back(item);
    for (int column = 0; column < 6; ++column)
        layout->setColumnStretch(column, column < columns ? 1 : 0);
    for (int index = 0; index < static_cast<int>(items.size()); ++index)
    {
        layout->addItem(items[index], index / columns, index % columns);
    }
}
}

void ReviaWindow::ApplyContentWidthCap()
{
    if (ui == nullptr || ui->rootLayout == nullptr)
    {
        return;
    }
    constexpr int readableFloor = 1500;
    constexpr int minimumSideMargin = 22;
    const int maximumContentWidth = std::max(readableFloor, static_cast<int>(static_cast<double>(width()) * 0.9));
    const int side = std::max(minimumSideMargin, (width() - maximumContentWidth) / 2);
    ui->rootLayout->setContentsMargins(side, 20, side, 20);
}

void ReviaWindow::ApplyResponsiveLayout()
{
    if (ui == nullptr || ui->statusLayout == nullptr || tabs == nullptr)
        return;
    if (navigationSidebar)
        navigationSidebar->SetNarrow(width() < 1000);
    const int contentWidth = width() - ui->rootLayout->contentsMargins().left() - ui->rootLayout->contentsMargins().right() -
                             (navigationSidebar ? navigationSidebar->width() + 16 : 0);
    const bool compact = contentWidth < 850;
    ReflowGrid(ui->statusLayout, compact ? 3 : 6);
    ReflowGrid(ui->activityToolbarLayout, compact ? 2 : 6);
    ReflowGrid(ui->conversationBehaviorControls, compact ? 1 : 2);
    if (auto* cards = ui->permissionsPage->findChild<QGridLayout*>("permissionCards"))
    {
        ReflowGrid(cards, contentWidth < 1150 ? 1 : 2);
    }
    for (auto* label : {affectLabel, speechLabel, microphoneLabel, automationLabel, visionLabel, perceptionLabel})
    {
        label->setMaximumWidth(QWIDGETSIZE_MAX);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    for (const auto* name :
        {"presenceToggleLayout", "preferenceLayout", "microphoneDeviceLayout", "diagnosticsLayout", "permissionApplications"})
    {
        if (auto* layout = findChild<QBoxLayout*>(name))
        {
            const bool settingsRow = layout == ui->preferenceLayout || layout == ui->microphoneDeviceLayout;
            const bool stack = settingsRow ? compact : contentWidth < 1150;
            layout->setDirection(stack ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        }
    }
    for (auto* form : findChildren<QFormLayout*>())
    {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    stateDetailLabel->setWordWrap(true);
    stateDetailLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    stateDetailLabel->setMinimumWidth(compact ? 100 : 160);
    stateDetailLabel->setMaximumWidth(compact ? 250 : 450);
    stateDetailLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    for (auto* label : {ui->avatarPathValue, ui->adapterInboxValue, ui->adapterOutboxValue})
    {
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    tabs->tabBar()->setDrawBase(false);
    tabs->tabBar()->setUsesScrollButtons(false);
    // Recompute tab metrics after grouping changes the bar's style selector.
    tabs->tabBar()->setStyleSheet(compact ? "QTabBar::tab { padding: 8px 10px; }" : "QTabBar::tab { padding: 9px 14px; }");
}

void ReviaWindow::BuildNavigationSidebar()
{
    auto* body = new QWidget(ui->rootPanel);
    body->setObjectName("navigationBody");
    auto* row = new QHBoxLayout(body);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);
    navigationSidebar = new NavigationSidebar(tabs, [this]() { ApplyResponsiveLayout(); }, body);
    row->addWidget(navigationSidebar);
    auto* content = new QWidget(body);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(12);
    for (QLayout* layout : {static_cast<QLayout*>(ui->headerLayout), static_cast<QLayout*>(ui->statusLayout)})
    {
        ui->rootLayout->removeItem(layout);
        layout->setParent(nullptr);
        column->addLayout(layout);
    }
    ui->rootLayout->removeWidget(tabs);
    column->addWidget(tabs, 1);
    row->addWidget(content, 1);
    ui->rootLayout->addWidget(body, 1);
    ui->reviaTitle->setStyleSheet("");
    const auto titleChanged = [this](const int index)
    {
        ui->reviaTitle->setText(tabs->tabText(index).section(" (", 0, 0));
    };
    connect(tabs, &QTabWidget::currentChanged, this, titleChanged);
    titleChanged(tabs->currentIndex());
    runtimeDetailsButton = new QPushButton("Details", content);
    runtimeDetailsButton->setObjectName("runtimeDetailsButton");
    runtimeDetailsButton->setToolTip("Show startup messages and the runtime log folder");
    runtimeDetailsButton->hide();
    ui->headerLayout->addWidget(runtimeDetailsButton);
    connect(runtimeDetailsButton, &QPushButton::clicked, this, [this]()
        {
            if (tabs->isEnabled())
                revia::desktop::SelectNavigationPage(ui->activityPage);
        });
}
