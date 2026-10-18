#include "chatMarkdown.h"

#include <QColor>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>

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
    QTextDocument document;
    document.setMarkdown(KeepLineBreaks(text), QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));

    std::vector<Rewrite> rewrites;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
    {
        const bool code = IsCodeBlock(block);
        const int heading = block.blockFormat().headingLevel();
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
                format.setForeground(QColor(QStringLiteral("#8fd3ff")));
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
                    format.setBackground(QColor(QStringLiteral("#1a2940")));
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
            plain.setForeground(QColor(QStringLiteral("#93a4bd")));
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
        if (IsCodeBlock(block))
        {
            format.setBackground(QColor(QStringLiteral("#0b1424")));
            format.setLeftMargin(6);
            format.setRightMargin(6);
        }
        blockCursor.setBlockFormat(format);
    }

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
