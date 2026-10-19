#include "composerCard.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace revia::desktop
{
ComposerCard::ComposerCard(QWidget* parent) : QFrame(parent)
{
    setObjectName("composerCard");
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("focused", false);
    setProperty("voice", voiceState);
    column = new QVBoxLayout(this);
    // The card's corners are 20px round: content keeps clear of them.
    column->setContentsMargins(14, 10, 10, 8);
    column->setSpacing(6);

    context = new QWidget(this);
    context->setObjectName("composerContext");
    context->setAttribute(Qt::WA_StyledBackground, true);
    auto* contextRow = new QHBoxLayout(context);
    contextRow->setContentsMargins(10, 4, 4, 4);
    contextRow->setSpacing(8);
    contextLabel = new QLabel(context);
    contextLabel->setObjectName("composerContextLabel");
    contextLabel->setTextFormat(Qt::PlainText);
    contextRow->addWidget(contextLabel, 1);
    contextAction = new QPushButton(context);
    contextAction->setObjectName("composerContextAction");
    contextAction->setFlat(true);
    contextAction->setFocusPolicy(Qt::TabFocus);
    connect(contextAction, &QPushButton::clicked, this,
        [this]()
        {
            // A copy: the action usually clears the context, which replaces the callback.
            const std::function<void()> action = contextCallback;
            if (action)
                action();
        });
    contextRow->addWidget(contextAction);
    context->hide();
    column->addWidget(context);

    toolbar = new QWidget(this);
    toolbar->setObjectName("composerToolbar");
    auto* toolbarRow = new QHBoxLayout(toolbar);
    toolbarRow->setContentsMargins(0, 0, 0, 0);
    toolbarRow->setSpacing(4);
    leading = new QHBoxLayout;
    leading->setSpacing(4);
    trailing = new QHBoxLayout;
    trailing->setSpacing(4);
    toolbarRow->addLayout(leading);
    toolbarRow->addStretch(1);
    toolbarRow->addLayout(trailing);
    column->addWidget(toolbar);
}

void ComposerCard::SetTray(QWidget* const widget)
{
    if (tray)
        column->removeWidget(tray);
    tray = widget;
    if (tray)
        column->insertWidget(column->indexOf(context) + 1, tray);
}

void ComposerCard::SetInput(QPlainTextEdit* const widget)
{
    if (input)
    {
        input->removeEventFilter(this);
        disconnect(input, nullptr, this, nullptr);
        disconnect(input->document()->documentLayout(), nullptr, this, nullptr);
        column->removeWidget(input);
    }
    input = widget;
    if (!input)
        return;
    column->insertWidget(column->indexOf(toolbar), input);
    input->setFrameShape(QFrame::NoFrame);
    input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    input->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Tab walks on to the toolbar buttons instead of typing a tab character.
    input->setTabChangesFocus(true);
    input->installEventFilter(this);
    connect(input, &QPlainTextEdit::textChanged, this, [this]() { UpdateInputHeight(); });
    // Also fires when a width change rewraps the text.
    connect(
        input->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this]() { UpdateInputHeight(); });
    SetFocused(input->hasFocus());
    ApplyPlaceholder();
    UpdateInputHeight();
}

void ComposerCard::AddLeading(QWidget* const widget)
{
    leading->addWidget(widget);
}

void ComposerCard::AddTrailing(QWidget* const widget)
{
    trailing->addWidget(widget);
}

void ComposerCard::InsertTrailing(const int index, QWidget* const widget)
{
    if (index < 0 || index > trailing->count())
        trailing->addWidget(widget);
    else
        trailing->insertWidget(index, widget);
}

int ComposerCard::TrailingIndexOf(QWidget* const widget) const
{
    return trailing->indexOf(widget);
}

void ComposerCard::SetMaxInputLines(const int lines)
{
    maxLines = std::max(1, lines);
    UpdateInputHeight();
}

void ComposerCard::SetMaxInputHeight(const int pixels)
{
    maxHeight = std::max(1, pixels);
    UpdateInputHeight();
}

void ComposerCard::SetContext(const QString& text, const QString& actionText, std::function<void()> action)
{
    contextLabel->setText(text);
    contextAction->setText(actionText);
    contextAction->setVisible(!actionText.isEmpty());
    contextCallback = std::move(action);
    context->show();
}

void ComposerCard::ClearContext()
{
    context->hide();
    contextLabel->clear();
    contextCallback = {};
}

void ComposerCard::SetVoiceState(const QString& state)
{
    const QString value = state.isEmpty() ? QStringLiteral("idle") : state.toLower();
    if (value == voiceState)
        return;
    voiceState = value;
    setProperty("voice", voiceState);
    Repolish();
    ApplyPlaceholder();
}

void ComposerCard::SetPlaceholder(const QString& text)
{
    explicitPlaceholder = true;
    placeholder = text;
    if (input && !showingVoicePlaceholder)
        input->setPlaceholderText(text);
}

void ComposerCard::ApplyPlaceholder()
{
    if (!input)
        return;
    QString voiceText;
    if (voiceState == QLatin1String("listening"))
        voiceText = QStringLiteral("Listening…");
    else if (voiceState == QLatin1String("transcribing"))
        voiceText = QStringLiteral("Transcribing…");
    if (!voiceText.isEmpty())
    {
        // Without SetPlaceholder, remember whatever the window put there last, fresh each time.
        if (!showingVoicePlaceholder && !explicitPlaceholder)
            placeholder = input->placeholderText();
        showingVoicePlaceholder = true;
        input->setPlaceholderText(voiceText);
    }
    else if (showingVoicePlaceholder)
    {
        showingVoicePlaceholder = false;
        input->setPlaceholderText(placeholder);
    }
}

void ComposerCard::UpdateInputHeight()
{
    if (!input)
        return;
    const QTextDocument* document = input->document();
    const QAbstractTextDocumentLayout* layout = document->documentLayout();
    const qreal margin = document->documentMargin();
    // QPlainTextEdit scrolls unless its viewport is a pixel taller than its blocks (the last one
    // carries the bottom margin) plus the top margin. Its lines are whole pixels tall.
    const QTextBlock first = document->firstBlock();
    const QRectF firstRect = layout->blockBoundingRect(first); // lays the block out
    qreal line = std::ceil(QFontMetricsF(input->font()).lineSpacing());
    if (first.layout() != nullptr && first.layout()->lineCount() > 0)
        line = first.layout()->lineAt(0).height();
    const QMargins margins = input->contentsMargins();
    const qreal chrome = margin + 1 + margins.top() + margins.bottom();
    const int minimum = static_cast<int>(std::ceil(line + margin + chrome));
    const int limit = std::max(minimum, std::min(static_cast<int>(std::ceil(maxLines * line + margin + chrome)), maxHeight));
    qreal blocks = firstRect.height();
    for (QTextBlock block = first.next(); block.isValid() && blocks + chrome < limit; block = block.next())
        blocks += layout->blockBoundingRect(block).height();
    const int height = std::clamp(static_cast<int>(std::ceil(blocks + chrome)), minimum, limit);
    if (input->height() != height || input->minimumHeight() != height)
        input->setFixedHeight(height);
}

void ComposerCard::SetFocused(const bool focused)
{
    if (property("focused").toBool() == focused)
        return;
    setProperty("focused", focused);
    Repolish();
}

void ComposerCard::Repolish()
{
    style()->unpolish(this);
    style()->polish(this);
    update();
}

bool ComposerCard::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == input)
    {
        switch (event->type())
        {
        case QEvent::FocusIn:
            SetFocused(true);
            break;
        case QEvent::FocusOut:
            SetFocused(false);
            break;
        case QEvent::FontChange:
        case QEvent::StyleChange:
            UpdateInputHeight();
            break;
        case QEvent::Resize:
            if (input->width() != lastInputWidth)
            {
                lastInputWidth = input->width();
                UpdateInputHeight();
            }
            break;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void ComposerCard::mousePressEvent(QMouseEvent* event)
{
    // A click on the card's padding or toolbar gaps puts the caret in the message box.
    if (input && input->isEnabled())
        input->setFocus(Qt::MouseFocusReason);
    QFrame::mousePressEvent(event);
}
}
