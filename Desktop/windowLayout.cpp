#include "reviaWindow.h"
#include "ui_reviaWindow.h"

#include "capabilityPanel.h"
#include "shellSidebar.h"

#include <QBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextFrameFormat>

#include <algorithm>
#include <vector>

namespace
{
void ReflowGrid(QGridLayout* layout, const int columns)
{
    if (layout == nullptr || layout->property("columns").toInt() == columns)
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

// Each window reflows on its own size (design spec 2.11). The main window: the sidebar falls
// back to its rail below 900px, where expanding it lasts only until the window widens, and the
// header drops the state's detail text below 640px of chat column.
void ReviaWindow::ApplyResponsiveLayout()
{
    if (navigationSidebar)
        navigationSidebar->SetNarrow(width() < 900);
    if (stateDetailLabel && chatColumn)
        stateDetailLabel->setVisible(!stateDetailLabel->text().isEmpty() && chatColumn->width() >= 640);
}

// Settings at its content width: the permission cards take two columns from 900px (the panel
// works that out from its own width), and the microphone row stacks below 560px.
void ReviaWindow::ApplySettingsLayout(const int contentWidth)
{
    if (capabilityPanel)
        capabilityPanel->ApplyContentWidth(contentWidth - 80);
    if (microphoneRow)
    {
        if (auto* row = dynamic_cast<QBoxLayout*>(microphoneRow->layout()))
            row->setDirection(contentWidth < 560 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    }
}

// The Activity window: its toolbar takes two columns below 640px, otherwise one row.
void ReviaWindow::ApplyActivityLayout(const int width)
{
    ReflowGrid(ui->activityToolbarLayout, width < 640 ? 2 : 6);
}

// The transcript reads at a 760px measure, centred, whatever the window's width. Only the root
// frame's margins change, so a resize never re-renders the conversation.
void ReviaWindow::ApplyChatMeasure()
{
    if (chatHistory == nullptr)
        return;
    QTextFrame* root = chatHistory->document()->rootFrame();
    QTextFrameFormat format = root->frameFormat();
    const qreal side = std::max(16, (chatHistory->viewport()->width() - 760) / 2);
    // An empty chat's greeting sits roughly a third of the way down, like a greeting rather than
    // a notice. It is the frame that moves: QTextDocument drops the top margin of its first block.
    const bool greeting = chatEntries.empty() && viewingArchivedChat.empty() && !lastTurnError;
    const qreal top = greeting ? std::max(24, chatHistory->viewport()->height() / 3 - 48) : 24;
    if (format.leftMargin() == side && format.rightMargin() == side && format.topMargin() == top && format.bottomMargin() == 24)
        return;
    format.setLeftMargin(side);
    format.setRightMargin(side);
    format.setTopMargin(top);
    format.setBottomMargin(24);
    root->setFrameFormat(format);
}

// The composer, the conversation strip and the saved-chat banner share the transcript's measure.
void ReviaWindow::ApplyComposerMeasure()
{
    if (chatColumn == nullptr)
        return;
    const int side = std::max(16, (chatColumn->width() - 760) / 2);
    if (composerArea != nullptr && composerArea->layout() != nullptr)
        composerArea->layout()->setContentsMargins(side, 0, side, 12);
    if (archivedBannerHost != nullptr && archivedBannerHost->layout() != nullptr)
        archivedBannerHost->layout()->setContentsMargins(side, 0, side, 0);
}
