#include "chatMarkdown.h"

#include "theme.h"

#include <QFont>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextTable>

#include <algorithm>
#include <vector>

namespace revia::desktop
{
namespace
{
const QStringList MonospaceFamilies{QStringLiteral("Consolas"), QStringLiteral("Cascadia Mono"), QStringLiteral("Courier New"),
    QStringLiteral("monospace")};

bool IsCodeBlock(const QTextBlock& block)
{
    const QTextBlockFormat format = block.blockFormat();
    return format.hasProperty(QTextFormat::BlockCodeFence) || format.hasProperty(QTextFormat::BlockCodeLanguage);
}

// A single line break in a reply is meant as one, as in any chat. Markdown would join
// the lines into one paragraph, so each is marked as a hard break; fenced code is left
// alone because it already keeps its lines.
QString KeepLineBreaks(const QString& text)
{
    QStringList lines = QString(text).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).split(QLatin1Char('\n'));
    bool fenced = false;
    for (qsizetype index = 0; index < lines.size(); ++index)
    {
        const QString trimmed = lines[index].trimmed();
        if (trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~")))
        {
            fenced = !fenced;
            continue;
        }
        const bool nextHasText = index + 1 < lines.size() && !lines[index + 1].trimmed().isEmpty();
        if (!fenced && nextHasText && !trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('|')) && !lines[index].endsWith(QStringLiteral("  ")))
            lines[index] += QStringLiteral("  ");
    }
    return lines.join(QLatin1Char('\n'));
}

// Hairline tables instead of Qt's default double outset borders and cramped cells: one 1px border
// colour, collapsed, with padding, and a quiet header row.
void StyleTables(QTextFrame* frame, const theme::Tokens& tokens)
{
    for (QTextFrame* child : frame->childFrames())
    {
        if (auto* table = dynamic_cast<QTextTable*>(child))
        {
            QTextTableFormat format = table->format();
            format.setBorder(1);
            format.setBorderBrush(tokens.border);
            format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
            format.setBorderCollapse(true);
            format.setCellSpacing(0);
            format.setCellPadding(6);
            format.setTopMargin(6);
            format.setBottomMargin(6);
            table->setFormat(format);
            // Cells read as rows of a grid, not as the reply's airy paragraphs.
            QTextCursor all = table->firstCursorPosition();
            all.setPosition(table->lastCursorPosition().position(), QTextCursor::KeepAnchor);
            QTextBlockFormat compact;
            compact.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
            all.mergeBlockFormat(compact);
            // Markdown tables always open with their header row.
            for (int column = 0; column < table->columns(); ++column)
            {
                QTextTableCell cell = table->cellAt(0, column);
                QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
                cellFormat.setBackground(tokens.surfaceAlt);
                cell.setFormat(cellFormat);
                QTextCursor contents = cell.firstCursorPosition();
                contents.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                QTextCharFormat weight;
                weight.setFontWeight(QFont::DemiBold);
                contents.mergeCharFormat(weight);
            }
        }
        StyleTables(child, tokens);
    }
}

// A fenced block becomes one framed box: a single-cell table with the code background, a hairline
// border and inner padding, rather than one grey strip per line with gaps between them.
void BoxCodeBlocks(QTextDocument& document, const theme::Tokens& tokens)
{
    struct Run
    {
        int start = 0;
        int end = 0;
        std::vector<QTextBlock> blocks;
    };
    std::vector<Run> runs;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
    {
        if (!IsCodeBlock(block) || QTextCursor(block).currentFrame() != document.rootFrame())
            continue;
        if (!runs.empty() && runs.back().blocks.back().next() == block)
            runs.back().blocks.push_back(block);
        else
            runs.push_back({0, 0, {block}});
    }
    QTextTableFormat box;
    box.setBackground(tokens.codeBackground);
    box.setBorder(1);
    box.setBorderBrush(tokens.codeBorder);
    box.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    box.setBorderCollapse(true);
    box.setCellPadding(10);
    box.setCellSpacing(0);
    box.setTopMargin(6);
    box.setBottomMargin(6);
    box.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    // From the end backwards, so the positions of the runs before stay valid.
    for (auto run = runs.rbegin(); run != runs.rend(); ++run)
    {
        std::vector<std::pair<QString, QTextBlockFormat>> lines;
        QTextCharFormat characters;
        bool haveCharacters = false;
        for (const QTextBlock& block : run->blocks)
        {
            QTextBlockFormat line = block.blockFormat();
            line.clearBackground();
            line.setTopMargin(0);
            line.setBottomMargin(0);
            line.setLeftMargin(0);
            line.setRightMargin(0);
            line.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
            lines.emplace_back(block.text(), line);
            if (!haveCharacters && block.begin() != block.end())
            {
                characters = block.begin().fragment().charFormat();
                haveCharacters = true;
            }
        }
        const QTextBlock first = run->blocks.front();
        const QTextBlock last = run->blocks.back();
        QTextCursor cursor(&document);
        cursor.setPosition(first.position());
        cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        // The block left behind holds the box and carries no code styling of its own.
        const int start = cursor.position();
        cursor.setBlockFormat(QTextBlockFormat());
        QTextTable* table = cursor.insertTable(1, 1, box);
        if (start > 0)
        {
            // That block is empty and would add a blank line above the box: fold it into the
            // paragraph before.
            QTextCursor fold(&document);
            fold.setPosition(start - 1);
            fold.setPosition(start, QTextCursor::KeepAnchor);
            fold.removeSelectedText();
        }
        QTextCursor cell = table->cellAt(0, 0).firstCursorPosition();
        for (std::size_t index = 0; index < lines.size(); ++index)
        {
            if (index == 0)
                cell.setBlockFormat(lines[index].second);
            else
                cell.insertBlock(lines[index].second, characters);
            cell.insertText(lines[index].first, characters);
        }
    }
}

// One change to the parsed document, applied from the end backwards so earlier
// positions stay valid while later text grows.
struct Rewrite
{
    int position = 0;
    int length = 0;
    QTextCharFormat format;
    bool replaceText = false;
    QString text;
    QString suffix;
};
}

QString RenderUntrustedMarkdown(const QString& text)
{
    // Colours are baked into the returned HTML, so the transcript re-renders on a theme change.
    const theme::Tokens& tokens = theme::Current();
    QTextDocument document;
    document.setMarkdown(KeepLineBreaks(text), QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));

    std::vector<Rewrite> rewrites;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
    {
        const bool code = IsCodeBlock(block);
        const int heading = block.blockFormat().headingLevel();
        const bool quote = block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0;
        for (auto fragmentIt = block.begin(); !fragmentIt.atEnd(); ++fragmentIt)
        {
            const QTextFragment fragment = fragmentIt.fragment();
            if (!fragment.isValid())
                continue;
            QTextCharFormat format = fragment.charFormat();
            Rewrite rewrite;
            rewrite.position = fragment.position();
            rewrite.length = fragment.length();
            bool changed = false;
            if (quote)
            {
                // A quoted passage reads as someone else's words: quieter than the reply.
                format.setForeground(tokens.textSecondary);
                changed = true;
            }
            if (heading > 0)
            {
                // A reply heading is a section label inside a chat bubble, not a page title.
                format.setProperty(QTextFormat::FontSizeAdjustment, heading <= 2 ? 1 : 0);
                changed = true;
            }
            if (format.isImageFormat())
            {
                // Never loaded: a picture the model names could be any file on this
                // computer. Its address is shown instead.
                const QString source = format.toImageFormat().name();
                QTextCharFormat plain;
                rewrite.format = plain;
                rewrite.replaceText = true;
                rewrite.text = source.isEmpty() ? QStringLiteral("[image]") : QStringLiteral("[image: ") + source + QStringLiteral("]");
                rewrites.push_back(rewrite);
                continue;
            }
            if (format.isAnchor() || format.hasProperty(QTextFormat::AnchorHref))
            {
                // Every link in the transcript that acts belongs to the window. A link in
                // a reply could otherwise imitate Copy, Edit or Regenerate.
                const QString target = format.anchorHref();
                format.clearProperty(QTextFormat::IsAnchor);
                format.clearProperty(QTextFormat::AnchorHref);
                format.clearProperty(QTextFormat::AnchorName);
                format.setForeground(tokens.link);
                format.setFontUnderline(true);
                const QString shown = fragment.text();
                if (!target.isEmpty() && !shown.contains(target) && !target.endsWith(shown))
                    rewrite.suffix = QStringLiteral(" (") + target + QStringLiteral(")");
                changed = true;
            }
            if (code || format.fontFixedPitch())
            {
                format.setFontFamilies(MonospaceFamilies);
                format.setFontFixedPitch(true);
                if (!code)
                    format.setBackground(tokens.codeBackground);
                changed = true;
            }
            if (changed)
            {
                rewrite.format = format;
                rewrites.push_back(rewrite);
            }
        }
    }

    QTextCursor cursor(&document);
    std::sort(rewrites.begin(), rewrites.end(), [](const Rewrite& left, const Rewrite& right) { return left.position > right.position; });
    for (const Rewrite& rewrite : rewrites)
    {
        cursor.setPosition(rewrite.position);
        cursor.setPosition(rewrite.position + rewrite.length, QTextCursor::KeepAnchor);
        if (rewrite.replaceText)
        {
            cursor.insertText(rewrite.text, rewrite.format);
            continue;
        }
        cursor.setCharFormat(rewrite.format);
        if (!rewrite.suffix.isEmpty())
        {
            QTextCharFormat plain = rewrite.format;
            plain.setFontUnderline(false);
            plain.setForeground(tokens.textSecondary);
            cursor.setPosition(rewrite.position + rewrite.length);
            cursor.insertText(rewrite.suffix, plain);
        }
    }

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
    {
        // Tighter than a document's paragraph spacing: these are lines in a chat.
        QTextCursor blockCursor(block);
        QTextBlockFormat format = block.blockFormat();
        format.setTopMargin(std::min<qreal>(format.topMargin(), 2));
        format.setBottomMargin(std::min<qreal>(format.bottomMargin(), 2));
        blockCursor.setBlockFormat(format);
    }
    StyleTables(document.rootFrame(), tokens);
    BoxCodeBlocks(document, tokens);

    // The body only: the document head carries its own default font and margins, which
    // would override the transcript's.
    const QString html = document.toHtml();
    const qsizetype bodyTag = html.indexOf(QStringLiteral("<body"));
    const qsizetype bodyStart = bodyTag < 0 ? -1 : html.indexOf(QLatin1Char('>'), bodyTag);
    const qsizetype bodyEnd = html.lastIndexOf(QStringLiteral("</body>"));
    if (bodyStart < 0 || bodyEnd <= bodyStart)
        return text.toHtmlEscaped();
    return html.mid(bodyStart + 1, bodyEnd - bodyStart - 1).trimmed();
}
}
