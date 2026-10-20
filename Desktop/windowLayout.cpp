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
    FitStateDetail();
}

void ReviaWindow::FitStateDetail()
{
    if (stateDetailLabel == nullptr || chatHeader == nullptr || chatHeader->layout() == nullptr)
        return;
    const QString full = stateDetailLabel->toolTip();
    int room = 0;
    if (!full.isEmpty() && (chatColumn == nullptr || chatColumn->width() >= 640))
    {
        // What is left once the title keeps its minimum and every other item (privacy indicators,
        // the desktop stop, the pill, the menu) its size. The label's own minimum is 1px, so its
        // text never sets the header's, and so the window's, minimum width.
        QLayout* layout = chatHeader->layout();
        const QMargins margins = layout->contentsMargins();
        int used = margins.left() + margins.right();
        for (int index = 0; index < layout->count(); ++index)
        {
            QLayoutItem* item = layout->itemAt(index);
            QWidget* widget = item->widget();
            if (widget == nullptr || widget->isHidden() || widget == stateDetailLabel)
                continue;
            // The layout item's hint, which honours a fixed size (the pill's).
            used += (widget == chatTitle ? chatTitle->minimumWidth() : item->sizeHint().width()) + layout->spacing();
        }
        room = std::min(280, chatHeader->width() - used);
    }
    const bool shown = room >= 48;
    if (shown)
        stateDetailLabel->setText(stateDetailLabel->fontMetrics().elidedText(full, Qt::ElideRight, room));
    if (stateDetailLabel->isHidden() == shown)
        stateDetailLabel->setVisible(shown);
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
// frame's margins change, so a resize never re-renders the conversation. The column is centred
// on the transcript's full width, as the composer is on the chat column's: a vertical scroll bar
// takes its width from the right margin, so the text keeps the composer's edges whether or not
// the bar shows (and keeps its width, so the bar cannot come and go with the margins).
void ReviaWindow::ApplyChatMeasure()
{
    if (chatHistory == nullptr)
        return;
    QTextFrame* root = chatHistory->document()->rootFrame();
    QTextFrameFormat format = root->frameFormat();
    const int full = chatHistory->width() - 2 * chatHistory->frameWidth();
    const int left = std::max(16, (full - 760) / 2);
    const int bar = std::max(0, full - chatHistory->viewport()->width());
    const qreal side = left;
    // At the narrowest widths the bar sits in the 16px gutter itself, beside the text's edge.
    const qreal right = std::max(0, left - bar);
    // An empty chat's greeting sits roughly a third of the way down, like a greeting rather than
    // a notice. It is the frame that moves: QTextDocument drops the top margin of its first block.
    const bool greeting = chatEntries.empty() && viewingArchivedChat.empty() && !lastTurnError;
    const qreal top = greeting ? std::max(24, chatHistory->viewport()->height() / 3 - 48) : 24;
    if (format.leftMargin() == side && format.rightMargin() == right && format.topMargin() == top && format.bottomMargin() == 24)
        return;
    format.setLeftMargin(side);
    format.setRightMargin(right);
    format.setTopMargin(top);
    format.setBottomMargin(24);
    root->setFrameFormat(format);
}

// The composer, the conversation strip and the saved-chat banner share the transcript's measure.
// The composer's shadow host spends its blur on margins around the card; those fall outside the
// measure, so the card's border lines up with the bubbles and code blocks above it (and the
// strip, padded by the same amount, keeps its text on that edge).
void ReviaWindow::ApplyComposerMeasure()
{
    if (chatColumn == nullptr)
        return;
    const int side = std::max(16, (chatColumn->width() - 760) / 2);
    const int shadow = composerShadow != nullptr && composerShadow->layout() != nullptr ? composerShadow->layout()->contentsMargins().left() : 0;
    if (composerArea != nullptr && composerArea->layout() != nullptr)
        composerArea->layout()->setContentsMargins(std::max(0, side - shadow), 0, std::max(0, side - shadow), 12);
    if (archivedBannerHost != nullptr && archivedBannerHost->layout() != nullptr)
        archivedBannerHost->layout()->setContentsMargins(side, 0, side, 0);
}
