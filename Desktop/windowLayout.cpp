#include "reviaWindow.h"
#include "ui_reviaWindow.h"

#include <QBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QTabBar>

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
    const bool compact = width() < 900;
    ReflowGrid(ui->statusLayout, compact ? 3 : 6);
    ReflowGrid(ui->activityToolbarLayout, compact ? 2 : 6);
    ReflowGrid(ui->conversationBehaviorControls, compact ? 1 : 2);
    if (auto* cards = ui->permissionsPage->findChild<QGridLayout*>("permissionCards"))
    {
        ReflowGrid(cards, width() < 1150 ? 1 : 2);
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
            const bool stack = settingsRow ? compact : width() < 1150;
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
    stateDetailLabel->setMaximumHeight(38);
    for (auto* label : {ui->avatarPathValue, ui->adapterInboxValue, ui->adapterOutboxValue})
    {
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    tabs->tabBar()->setDrawBase(false);
    tabs->tabBar()->setUsesScrollButtons(true);
    tabs->tabBar()->setStyleSheet(compact ? "QTabBar::tab { padding: 8px 10px; }" : "");
}
