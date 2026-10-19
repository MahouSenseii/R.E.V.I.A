#include "theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QFont>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace revia::desktop::theme
{
namespace
{
QColor Hex(const char* value)
{
    return QColor(QLatin1String(value));
}

QColor Rgba(const int red, const int green, const int blue, const double alpha)
{
    QColor colour(red, green, blue);
    colour.setAlphaF(alpha);
    return colour;
}

// Light values keep text at AA on every surface it sits on, including hover and selected rows;
// the darkest accent, link and tertiary text values exist for that reason (see ContrastPairs).
Tokens MakeLight()
{
    Tokens tokens;
    tokens.scheme = Scheme::Light;
    tokens.background = Hex("#FAF9F7");
    tokens.sidebar = Hex("#F3F1EC");
    tokens.sidebarHover = Hex("#EAE7E0");
    tokens.sidebarSelected = Hex("#E4E0D7");
    tokens.surface = Hex("#FFFFFF");
    tokens.surfaceAlt = Hex("#F3F1EC");
    tokens.surfaceHover = Hex("#F0EEE9");
    tokens.inputBackground = Hex("#FFFFFF");
    tokens.border = Hex("#E6E3DC");
    tokens.borderStrong = Hex("#D3CEC4");
    tokens.inputBorder = Hex("#8C877D");
    tokens.divider = Hex("#EDEAE4");
    tokens.text = Hex("#1F1E1C");
    tokens.textSecondary = Hex("#57534E");
    tokens.textTertiary = Hex("#67635C");
    tokens.textDisabled = Hex("#A9A49B");
    tokens.accent = Hex("#0D6B64");
    tokens.accentHover = Hex("#0B5F59");
    tokens.accentPressed = Hex("#094F4A");
    tokens.accentSoft = Hex("#E3F2EF");
    tokens.textOnAccent = Hex("#FFFFFF");
    tokens.selection = Hex("#CDEBE6");
    tokens.selectionText = Hex("#1F1E1C");
    tokens.focusRing = Hex("#0D6B64");
    tokens.link = Hex("#0D6B64");
    tokens.danger = Hex("#B42318");
    tokens.dangerSoft = Hex("#FDECEA");
    tokens.warning = Hex("#B54708");
    tokens.warningSoft = Hex("#FEF3E2");
    tokens.success = Hex("#067647");
    tokens.successSoft = Hex("#E6F4EC");
    tokens.userBubble = Hex("#F0EEE9");
    tokens.codeBackground = Hex("#F3F1EC");
    tokens.codeBorder = Hex("#E6E3DC");
    tokens.scrollbar = Hex("#D3CEC4");
    tokens.scrollbarHover = Hex("#B9B3A8");
    tokens.switchOff = Hex("#736F68");
    tokens.scrim = Rgba(31, 30, 28, 0.32);
    tokens.shadow = Rgba(28, 25, 23, 0.14);
    return tokens;
}

Tokens MakeDark()
{
    Tokens tokens;
    tokens.scheme = Scheme::Dark;
    tokens.background = Hex("#1F1E1C");
    tokens.sidebar = Hex("#181816");
    tokens.sidebarHover = Hex("#262522");
    tokens.sidebarSelected = Hex("#2E2D2A");
    tokens.surface = Hex("#2A2926");
    tokens.surfaceAlt = Hex("#262522");
    tokens.surfaceHover = Hex("#33322E");
    tokens.inputBackground = Hex("#2A2926");
    tokens.border = Hex("#3A3935");
    tokens.borderStrong = Hex("#4C4A45");
    tokens.inputBorder = Hex("#7A766E");
    tokens.divider = Hex("#2F2E2B");
    tokens.text = Hex("#ECEAE4");
    tokens.textSecondary = Hex("#B5B1A8");
    tokens.textTertiary = Hex("#A39F96");
    tokens.textDisabled = Hex("#6A675F");
    tokens.accent = Hex("#4FD1C5");
    tokens.accentHover = Hex("#6EDBD1");
    tokens.accentPressed = Hex("#3BB8AD");
    tokens.accentSoft = Hex("#1D3A37");
    tokens.textOnAccent = Hex("#0E1F1D");
    tokens.selection = Hex("#1F4A46");
    tokens.selectionText = Hex("#ECEAE4");
    tokens.focusRing = Hex("#4FD1C5");
    tokens.link = Hex("#5FD8CC");
    tokens.danger = Hex("#F97066");
    tokens.dangerSoft = Hex("#3A1F1C");
    tokens.warning = Hex("#FDB022");
    tokens.warningSoft = Hex("#3A2C14");
    tokens.success = Hex("#47CD89");
    tokens.successSoft = Hex("#17311F");
    tokens.userBubble = Hex("#33322E");
    tokens.codeBackground = Hex("#262522");
    tokens.codeBorder = Hex("#3A3935");
    tokens.scrollbar = Hex("#4C4A45");
    tokens.scrollbarHover = Hex("#625F59");
    tokens.switchOff = Hex("#9A968D");
    tokens.scrim = Rgba(0, 0, 0, 0.55);
    tokens.shadow = Rgba(0, 0, 0, 0.50);
    return tokens;
}

// Under high contrast the system palette decides every colour. Roles without a system
// counterpart (status tones, soft fills) collapse to text and window colours, which the
// high-contrast palette guarantees to read against each other.
Tokens TokensFromPalette(const QPalette& palette)
{
    const QColor window = palette.color(QPalette::Active, QPalette::Window);
    const QColor base = palette.color(QPalette::Active, QPalette::Base);
    const QColor text = palette.color(QPalette::Active, QPalette::WindowText);
    const QColor buttonText = palette.color(QPalette::Active, QPalette::ButtonText);
    const QColor highlight = palette.color(QPalette::Active, QPalette::Highlight);
    const QColor highlightedText = palette.color(QPalette::Active, QPalette::HighlightedText);
    Tokens tokens;
    tokens.scheme = window.lightnessF() < 0.5 ? Scheme::Dark : Scheme::Light;
    tokens.background = tokens.sidebar = tokens.surfaceAlt = window;
    tokens.sidebarHover = tokens.surfaceHover = tokens.accentSoft = window;
    tokens.surface = tokens.inputBackground = tokens.userBubble = tokens.codeBackground = base;
    tokens.dangerSoft = tokens.warningSoft = tokens.successSoft = base;
    tokens.sidebarSelected = tokens.selection = highlight;
    tokens.border = tokens.borderStrong = tokens.inputBorder = tokens.divider = tokens.codeBorder = text;
    tokens.text = tokens.danger = tokens.warning = tokens.success = text;
    tokens.textSecondary = tokens.textTertiary = tokens.scrollbar = tokens.scrollbarHover = tokens.switchOff = buttonText;
    tokens.textDisabled = palette.color(QPalette::Disabled, QPalette::WindowText);
    tokens.accent = tokens.accentHover = tokens.accentPressed = tokens.focusRing = highlight;
    tokens.textOnAccent = tokens.selectionText = highlightedText;
    tokens.link = palette.color(QPalette::Active, QPalette::Link);
    tokens.scrim = Rgba(0, 0, 0, 0.5);
    tokens.shadow = palette.color(QPalette::Active, QPalette::Shadow);
    return tokens;
}

// The template placeholders, by token name.
struct TokenField
{
    const char* name;
    QColor Tokens::* member;
};

constexpr TokenField TokenFields[] = {
    {"background", &Tokens::background}, {"sidebar", &Tokens::sidebar}, {"sidebarHover", &Tokens::sidebarHover},
    {"sidebarSelected", &Tokens::sidebarSelected}, {"surface", &Tokens::surface}, {"surfaceAlt", &Tokens::surfaceAlt},
    {"surfaceHover", &Tokens::surfaceHover}, {"inputBackground", &Tokens::inputBackground}, {"border", &Tokens::border},
    {"borderStrong", &Tokens::borderStrong}, {"inputBorder", &Tokens::inputBorder}, {"divider", &Tokens::divider},
    {"text", &Tokens::text}, {"textSecondary", &Tokens::textSecondary}, {"textTertiary", &Tokens::textTertiary},
    {"textDisabled", &Tokens::textDisabled}, {"accent", &Tokens::accent}, {"accentHover", &Tokens::accentHover},
    {"accentPressed", &Tokens::accentPressed}, {"accentSoft", &Tokens::accentSoft}, {"textOnAccent", &Tokens::textOnAccent},
    {"selection", &Tokens::selection}, {"selectionText", &Tokens::selectionText}, {"focusRing", &Tokens::focusRing},
    {"link", &Tokens::link}, {"danger", &Tokens::danger}, {"dangerSoft", &Tokens::dangerSoft}, {"warning", &Tokens::warning},
    {"warningSoft", &Tokens::warningSoft}, {"success", &Tokens::success}, {"successSoft", &Tokens::successSoft},
    {"userBubble", &Tokens::userBubble}, {"codeBackground", &Tokens::codeBackground}, {"codeBorder", &Tokens::codeBorder},
    {"scrollbar", &Tokens::scrollbar}, {"scrollbarHover", &Tokens::scrollbarHover}, {"switchOff", &Tokens::switchOff},
    {"scrim", &Tokens::scrim}, {"shadow", &Tokens::shadow},
};

bool SameTokens(const Tokens& first, const Tokens& second)
{
    return first.scheme == second.scheme && std::all_of(std::begin(TokenFields), std::end(TokenFields),
        [&first, &second](const TokenField& field) { return first.*(field.member) == second.*(field.member); });
}

// Opaque colours as #rrggbb, anything translucent as rgba() with a 0-255 alpha, which both
// Qt's stylesheet parser and its rich-text CSS accept.
QString ColourText(const QColor& colour)
{
    if (colour.alpha() == 255)
        return colour.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(colour.red()).arg(colour.green()).arg(colour.blue()).arg(colour.alpha());
}

QString FillTemplate(const QString& text, const Tokens& tokens)
{
    static const QRegularExpression placeholder(QStringLiteral(R"(\{\{\s*(\w+)(?:/(\d{1,3}))?\s*\}\})"));
    QString result;
    result.reserve(text.size() + text.size() / 8);
    qsizetype last = 0;
    auto matches = placeholder.globalMatch(text);
    while (matches.hasNext())
    {
        const QRegularExpressionMatch match = matches.next();
        result += QStringView(text).mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const QString name = match.captured(1);
        if (name == QLatin1String("scheme"))
        {
            result += tokens.scheme == Scheme::Dark ? QStringLiteral("dark") : QStringLiteral("light");
            continue;
        }
        const auto field = std::find_if(std::begin(TokenFields), std::end(TokenFields),
            [&name](const TokenField& candidate) { return name == QLatin1String(candidate.name); });
        if (field == std::end(TokenFields))
        {
            result += match.captured(0);
            continue;
        }
        QColor colour = tokens.*(field->member);
        if (match.capturedLength(2) > 0)
            colour.setAlphaF(std::clamp(match.captured(2).toInt(), 0, 100) / 100.0);
        result += ColourText(colour);
    }
    result += QStringView(text).mid(last);
    Q_ASSERT_X(!result.contains(QLatin1String("{{")), "theme::StyleSheet", "reviaTheme.qss names a token theme.cpp does not know");
    return result;
}

QString ReadResource(const char* path)
{
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QPalette BuildPalette(const Tokens& tokens)
{
    QPalette palette;
    const auto set = [&palette](const QPalette::ColorRole role, const QColor& normal, const QColor& disabled)
    {
        // Inactive matches Active, so an unfocused window keeps its selection and text colours.
        palette.setColor(QPalette::Active, role, normal);
        palette.setColor(QPalette::Inactive, role, normal);
        palette.setColor(QPalette::Disabled, role, disabled);
    };
    set(QPalette::Window, tokens.background, tokens.background);
    set(QPalette::WindowText, tokens.text, tokens.textDisabled);
    set(QPalette::Base, tokens.inputBackground, tokens.surfaceAlt);
    set(QPalette::AlternateBase, tokens.surfaceAlt, tokens.surfaceAlt);
    set(QPalette::Text, tokens.text, tokens.textDisabled);
    set(QPalette::Button, tokens.surface, tokens.surfaceAlt);
    set(QPalette::ButtonText, tokens.text, tokens.textDisabled);
    set(QPalette::BrightText, tokens.textOnAccent, tokens.textOnAccent);
    set(QPalette::Highlight, tokens.selection, tokens.surfaceHover);
    set(QPalette::HighlightedText, tokens.selectionText, tokens.textDisabled);
    set(QPalette::Link, tokens.link, tokens.textDisabled);
    set(QPalette::LinkVisited, tokens.link, tokens.textDisabled);
    set(QPalette::ToolTipBase, tokens.surface, tokens.surface);
    set(QPalette::ToolTipText, tokens.text, tokens.text);
    set(QPalette::PlaceholderText, tokens.textSecondary, tokens.textDisabled);
    set(QPalette::Light, tokens.surface, tokens.surface);
    set(QPalette::Midlight, tokens.surfaceAlt, tokens.surfaceAlt);
    set(QPalette::Mid, tokens.borderStrong, tokens.border);
    set(QPalette::Dark, tokens.inputBorder, tokens.border);
    set(QPalette::Shadow, tokens.shadow, tokens.shadow);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    set(QPalette::Accent, tokens.accent, tokens.textDisabled);
#endif
    return palette;
}

// ---------------------------------------------------------------- icons

// Line icons on a 24-unit grid, drawn with a 1.75 stroke and round caps and joins.
// "currentColor" is replaced by the tone, for the few shapes that are filled.
struct IconShape
{
    const char* name;
    const char* elements;
};

constexpr IconShape IconShapes[] = {
    {"plus", "<path d='M12 5v14M5 12h14'/>"},
    {"search", "<circle cx='11' cy='11' r='7'/><path d='m20 20-3.5-3.5'/>"},
    {"panel-left", "<rect x='3' y='3' width='18' height='18' rx='2'/><path d='M9 3v18'/>"},
    {"settings", "<circle cx='12' cy='12' r='3'/><circle cx='12' cy='12' r='7'/>"
                 "<path d='M12 2v3M12 19v3M2 12h3M19 12h3M4.93 4.93l2.12 2.12M16.95 16.95l2.12 2.12M4.93 19.07l2.12-2.12M16.95 7.05l2.12-2.12'/>"},
    {"grid", "<rect x='3' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='3' width='7' height='7' rx='1.5'/>"
             "<rect x='3' y='14' width='7' height='7' rx='1.5'/><rect x='14' y='14' width='7' height='7' rx='1.5'/>"},
    {"message", "<path d='M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z'/>"},
    {"paperclip", "<path d='m21.44 11.05-9.19 9.19a6 6 0 0 1-8.49-8.49l8.57-8.57A4 4 0 1 1 18 8.84l-8.59 8.57a2 2 0 0 1-2.83-2.83l8.49-8.48'/>"},
    {"file-text", "<path d='M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z'/><path d='M14 3v5h5M9 13h6M9 17h6M9 9h1'/>"},
    {"monitor", "<rect x='2' y='3' width='20' height='14' rx='2'/><path d='M8 21h8M12 17v4'/>"},
    {"mic", "<rect x='9' y='2' width='6' height='12' rx='3'/><path d='M19 10v1a7 7 0 0 1-14 0v-1M12 18v4M8 22h8'/>"},
    {"mic-off", "<path d='m2 2 20 20'/><path d='M15 9.34V5a3 3 0 0 0-5.68-1.33M9 9v2a3 3 0 0 0 5.12 2.12'/>"
                "<path d='M16.95 16.95A7 7 0 0 1 5 11v-1M19 10v1a7 7 0 0 1-.11 1.23M12 18v4M8 22h8'/>"},
    {"square", "<rect x='7' y='7' width='10' height='10' rx='1.5' fill='currentColor'/>"},
    {"arrow-up", "<path d='M12 19V5M5 12l7-7 7 7'/>"},
    {"arrow-down", "<path d='M12 5v14M19 12l-7 7-7-7'/>"},
    {"chevron-down", "<path d='m6 9 6 6 6-6'/>"},
    {"chevron-right", "<path d='m9 6 6 6-6 6'/>"},
    {"more", "<circle cx='5' cy='12' r='1.25' fill='currentColor'/><circle cx='12' cy='12' r='1.25' fill='currentColor'/>"
             "<circle cx='19' cy='12' r='1.25' fill='currentColor'/>"},
    {"copy", "<rect x='9' y='9' width='12' height='12' rx='2'/><path d='M15 9V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h4'/>"},
    {"refresh", "<path d='M21 12a9 9 0 0 1-15.36 6.36L3 16M3 12a9 9 0 0 1 15.36-6.36L21 8'/><path d='M21 3v5h-5M3 21v-5h5'/>"},
    {"edit", "<path d='M12 20h9'/><path d='M16.5 3.5a2.12 2.12 0 0 1 3 3L7 19l-4 1 1-4z'/>"},
    {"check", "<path d='M20 6 9 17l-5-5'/>"},
    {"x", "<path d='M18 6 6 18M6 6l12 12'/>"},
    {"alert", "<path d='M10.29 3.86 1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z'/><path d='M12 9v4M12 17h.01'/>"},
    {"info", "<circle cx='12' cy='12' r='10'/><path d='M12 16v-4M12 8h.01'/>"},
    {"external", "<path d='M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6M15 3h6v6M10 14 21 3'/>"},
    {"sun", "<circle cx='12' cy='12' r='4'/>"
            "<path d='M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41'/>"},
    {"moon", "<path d='M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z'/>"},
    {"laptop", "<rect x='4' y='4' width='16' height='12' rx='2'/><path d='M2 20h20'/>"},
    {"image", "<rect x='3' y='3' width='18' height='18' rx='2'/><circle cx='9' cy='9' r='2'/><path d='m21 15-3.09-3.09a2 2 0 0 0-2.82 0L6 21'/>"},
    {"eye", "<path d='M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7z'/><circle cx='12' cy='12' r='3'/>"},
    {"camera", "<path d='M14.5 4h-5L7 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3z'/><circle cx='12' cy='13' r='3'/>"},
    {"cursor", "<path d='m4 4 7.07 17 2.51-7.39L21 11.07z'/>"},
    {"user", "<circle cx='12' cy='8' r='4'/><path d='M4 21v-1a6 6 0 0 1 6-6h4a6 6 0 0 1 6 6v1'/>"},
    {"brain", "<path d='M12 5a3 3 0 0 0-5.6-1.5A3 3 0 0 0 4 8a3 3 0 0 0 0 5 3 3 0 0 0 2.5 5A3 3 0 0 0 12 19z'/>"
              "<path d='M12 5a3 3 0 0 1 5.6-1.5A3 3 0 0 1 20 8a3 3 0 0 1 0 5 3 3 0 0 1-2.5 5A3 3 0 0 1 12 19z'/><path d='M12 5v14'/>"},
    {"database", "<ellipse cx='12' cy='5' rx='8' ry='3'/><path d='M4 5v14c0 1.66 3.58 3 8 3s8-1.34 8-3V5'/><path d='M4 12c0 1.66 3.58 3 8 3s8-1.34 8-3'/>"},
    {"waveform", "<path d='M2 10v4M6 6v12M10 3v18M14 8v8M18 5v14M22 10v4'/>"},
    {"sparkle", "<path d='m12 3 1.9 5.1L19 10l-5.1 1.9L12 17l-1.9-5.1L5 10l5.1-1.9z'/><path d='M19 16v5M16.5 18.5h5'/>"},
    {"users", "<circle cx='9' cy='8' r='4'/><path d='M2 21v-1a5 5 0 0 1 5-5h4a5 5 0 0 1 5 5v1'/><path d='M16 3.13a4 4 0 0 1 0 7.75M22 21v-1a5 5 0 0 0-3-4.58'/>"},
    {"bot", "<rect x='4' y='8' width='16' height='12' rx='2'/><path d='M12 8V4M9 4h6M9 13v2M15 13v2M2 14h2M20 14h2'/>"},
    {"graduation", "<path d='M22 10 12 5 2 10l10 5z'/><path d='M6 12v5c3 2.5 9 2.5 12 0v-5M22 10v6'/>"},
    {"code", "<path d='m16 18 6-6-6-6M8 6l-6 6 6 6'/>"},
    {"activity", "<path d='M22 12h-4l-3 9L9 3l-3 9H2'/>"},
    {"git-branch", "<path d='M6 3v12'/><circle cx='18' cy='6' r='3'/><circle cx='6' cy='18' r='3'/><path d='M18 9a9 9 0 0 1-9 9'/>"},
    {"globe", "<circle cx='12' cy='12' r='10'/><path d='M2 12h20'/><path d='M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z'/>"},
    {"cpu", "<rect x='5' y='5' width='14' height='14' rx='2'/><rect x='9' y='9' width='6' height='6' rx='1'/>"
            "<path d='M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3'/>"},
    {"shield", "<path d='M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z'/>"},
    {"keyboard", "<rect x='2' y='5' width='20' height='14' rx='2'/><path d='M6 9h.01M10 9h.01M14 9h.01M18 9h.01M6 13h.01M10 13h.01M14 13h.01M18 13h.01M8 16h8'/>"},
    {"columns", "<rect x='3' y='3' width='18' height='18' rx='2'/><path d='M12 3v18'/>"},
};

const IconShape* FindShape(const QString& name)
{
    const auto found = std::find_if(std::begin(IconShapes), std::end(IconShapes),
        [&name](const IconShape& shape) { return name == QLatin1String(shape.name); });
    return found == std::end(IconShapes) ? nullptr : found;
}

QPixmap RenderSvg(const QString& elements, const QColor& colour, const int logicalSize, const qreal ratio)
{
    const QString hex = colour.name(QColor::HexRgb);
    QString body = elements;
    body.replace(QLatin1String("currentColor"), hex);
    const QByteArray document = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%1' "
                                               "stroke-width='1.75' stroke-linecap='round' stroke-linejoin='round'>%2</svg>")
                                    .arg(hex, body)
                                    .toUtf8();
    const int pixels = std::max(1, static_cast<int>(std::ceil(logicalSize * ratio)));
    QPixmap pixmap(pixels, pixels);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setOpacity(colour.alphaF());
    QSvgRenderer renderer(document);
    renderer.render(&painter, QRectF(0, 0, logicalSize, logicalSize));
    painter.end();
    return pixmap;
}

// ---------------------------------------------------------------- state

struct ButtonBinding
{
    QPointer<QAbstractButton> button;
    std::string name;
    int size = 18;
};

struct ActionBinding
{
    QPointer<QAction> action;
    std::string name;
};

struct LabelBinding
{
    QPointer<QLabel> label;
    QString elements;
    Role role = Role::TextSecondary;
    int size = 18;
};

struct State
{
    bool installed = false;
    bool preferenceKnown = false;
    Preference preference = Preference::System;
    bool applied = false;
    bool applying = false;
    bool recomputePending = false;
    Scheme scheme = Scheme::Light;
    bool highContrast = false;
    std::optional<bool> highContrastOverride;
    Tokens light = MakeLight();
    Tokens dark = MakeDark();
    Tokens contrast = MakeLight();
    std::vector<QPointer<QWidget>> roots;
    QPointer<QObject> watcher;
    QPointer<QObject> focusTracker;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    std::vector<ButtonBinding> buttons;
    std::vector<ActionBinding> actions;
    std::vector<LabelBinding> labels;
    std::map<QString, QIcon> icons;
    QString structureSheet;
    QString colourSheet;
    bool sheetsLoaded = false;
};

// Never destroyed: icons, pixmaps and QPointers must not outlive QApplication in a static
// destructor, and the module is needed until the last window closes.
State& Get()
{
    static State* const state = new State();
    return *state;
}

void LoadSheets()
{
    State& state = Get();
    if (state.sheetsLoaded)
        return;
    state.structureSheet = ReadResource(":/revia/revia.qss");
    state.colourSheet = ReadResource(":/revia/reviaTheme.qss");
    state.sheetsLoaded = true;
}

bool SystemHighContrast()
{
#ifdef Q_OS_WIN
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) != FALSE &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
#else
    return false;
#endif
}

Scheme EffectiveScheme(const Preference preference)
{
    switch (preference)
    {
        case Preference::Light: return Scheme::Light;
        case Preference::Dark:  return Scheme::Dark;
        case Preference::System: break;
    }
    return SystemScheme();
}

bool IsPreferenceName(const QString& text)
{
    return text == QLatin1String("system") || text == QLatin1String("light") || text == QLatin1String("dark");
}

// REVIA_THEME first (tests and --ui-theme), then the stored choice, then System.
Preference InitialPreference()
{
    const QString forced = qEnvironmentVariable("REVIA_THEME").trimmed().toLower();
    if (IsPreferenceName(forced))
        return PreferenceFromString(forced);
    return PreferenceFromString(QSettings().value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString());
}

void EnsurePreference()
{
    State& state = Get();
    if (state.preferenceKnown)
        return;
    state.preference = InitialPreference();
    state.preferenceKnown = true;
}

#ifdef Q_OS_WIN
using SetWindowAttributeFunction = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

// dwmapi is loaded on first use, so a missing or old DWM only means plain chrome.
SetWindowAttributeFunction WindowAttributeSetter()
{
    static const SetWindowAttributeFunction setter = []() -> SetWindowAttributeFunction
    {
        const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm == nullptr)
            return nullptr;
        // Through void(*)(), which converts to any function pointer without a cast warning.
        return reinterpret_cast<SetWindowAttributeFunction>(
            reinterpret_cast<void (*)()>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    }();
    return setter;
}

COLORREF ColourRef(const QColor& colour)
{
    return RGB(colour.red(), colour.green(), colour.blue());
}
#endif

// Qt 6.8 can tell the platform which scheme the app shows, so native dialogs and the caption
// follow an override. System hands the decision back to Windows.
void UpdateColorSchemeOverride()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    State& state = Get();
    if (QGuiApplication::instance() == nullptr)
        return;
    QStyleHints* const hints = QGuiApplication::styleHints();
    switch (state.preference)
    {
        case Preference::Light: hints->setColorScheme(Qt::ColorScheme::Light); break;
        case Preference::Dark:  hints->setColorScheme(Qt::ColorScheme::Dark); break;
        case Preference::System: hints->unsetColorScheme(); break;
    }
#endif
}

void Reicon()
{
    State& state = Get();
    state.icons.clear();
    std::erase_if(state.buttons, [](const ButtonBinding& binding) { return binding.button.isNull(); });
    std::erase_if(state.actions, [](const ActionBinding& binding) { return binding.action.isNull(); });
    std::erase_if(state.labels, [](const LabelBinding& binding) { return binding.label.isNull(); });
    for (const ButtonBinding& binding : state.buttons)
        binding.button->setIcon(Icon(binding.name.c_str(), binding.size));
    for (const ActionBinding& binding : state.actions)
        binding.action->setIcon(Icon(binding.name.c_str(), 16));
    for (const LabelBinding& binding : state.labels)
    {
        const qreal ratio = std::max<qreal>(binding.label->devicePixelRatioF(), 2.0);
        binding.label->setPixmap(RenderSvg(binding.elements, RoleColor(binding.role), binding.size, ratio));
    }
}

void RunListeners()
{
    // By id, looked up each time: a listener may remove itself or another one while it runs.
    std::vector<int> ids;
    for (const auto& [id, listener] : Get().listeners)
        ids.push_back(id);
    for (const int id : ids)
    {
        const auto found = Get().listeners.find(id);
        if (found == Get().listeners.end())
            continue;
        const std::function<void()> listener = found->second;
        if (listener)
            listener();
    }
}

void ApplyNow()
{
    State& state = Get();
    if (state.applying)
        return;
    state.applying = true;
    UpdateColorSchemeOverride();
    const bool wasHighContrast = state.applied && state.highContrast;
    state.highContrast = state.highContrastOverride.value_or(SystemHighContrast());
    state.scheme = EffectiveScheme(state.preference);
    state.applied = true;

    if (state.highContrast)
    {
        // An empty palette resolves to the platform's, which carries the high-contrast colours
        // and keeps following them when Windows moves to another contrast theme, so it is set
        // once on the way in.
        if (!wasHighContrast)
            QApplication::setPalette(QPalette());
        state.contrast = TokensFromPalette(QApplication::palette());
    }
    else
    {
        QApplication::setPalette(BuildPalette(Current()));
    }

    // Set even when the text is unchanged: colours the sheet takes from the palette (a border
    // given no colour, a palette() reference) are resolved when a widget is polished.
    const QString sheet = StyleSheet();
    std::erase_if(state.roots, [](const QPointer<QWidget>& root) { return root.isNull(); });
    for (const QPointer<QWidget>& root : state.roots)
        root->setStyleSheet(sheet);
#ifdef Q_OS_WIN
    // Every top-level with a native window, shown or not: panel windows and the Settings dialog
    // are hidden on close, never deleted, and DWM keeps a caption colour on a window across hide
    // and show. One never shown has no native window yet; its show hook frames it.
    for (QWidget* const window : QApplication::topLevelWidgets())
        if (window->internalWinId() != 0 && (window->windowType() == Qt::Window || window->windowType() == Qt::Dialog))
            ApplyNativeFrame(window);
#endif
    Reicon();
    state.applying = false;
    RunListeners();
}

// Whether what is applied no longer matches what should be: the scheme or the high-contrast
// state changed, or Windows moved from one contrast theme to another (Night sky to Desert, say),
// where the high-contrast flag stays set and only the system palette changes. Qt reports our
// own setColorScheme() back as a change and a restyle costs a few hundred milliseconds, so the
// theme is applied only when this says so.
bool OutOfDate()
{
    const State& state = Get();
    if (!state.applied)
        return true;
    const bool highContrast = state.highContrastOverride.value_or(SystemHighContrast());
    if (highContrast != state.highContrast || EffectiveScheme(state.preference) != state.scheme)
        return true;
    return highContrast && !SameTokens(TokensFromPalette(QApplication::palette()), state.contrast);
}

void Recompute()
{
    State& state = Get();
    state.recomputePending = false;
    if (!state.installed || state.applying)
        return;
    if (OutOfDate())
        ApplyNow();
}

void ScheduleRecompute()
{
    State& state = Get();
    if (state.recomputePending || QCoreApplication::instance() == nullptr)
        return;
    state.recomputePending = true;
    QTimer::singleShot(0, QCoreApplication::instance(), []() { Recompute(); });
}

// Windows sends ThemeChange to top-level windows when the app mode or high contrast changes,
// on every Qt version, so each root reports it.
class ThemeWatcher final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::ThemeChange)
            ScheduleRecompute();
        return QObject::eventFilter(watched, event);
    }
};

// A click focuses an item view as well as the keyboard does, and a ring after every click on a
// list is noise, so revia.qss draws an item view's ring only while kbdFocus is set. One filter on
// the application keeps that property for every item view, whoever built it.
class KeyboardFocusTracker final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        {
            if (auto* const view = qobject_cast<QAbstractItemView*>(watched))
            {
                const bool before = view->property("kbdFocus").toBool();
                const bool after = KeyboardFocusAfter(before, *static_cast<QFocusEvent*>(event));
                if (after != before)
                {
                    // Property selectors are matched at polish time.
                    view->setProperty("kbdFocus", after);
                    view->style()->unpolish(view);
                    view->style()->polish(view);
                    view->update();
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void InstallOnce()
{
    State& state = Get();
    if (state.installed)
        return;
    state.installed = true;
    // Fusion follows the palette and stylesheet the same way on every platform; the native
    // Windows styles ignore parts of both.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font = QApplication::font();
    font.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI")});
    QApplication::setFont(font);
    state.watcher = new ThemeWatcher(QCoreApplication::instance());
    state.focusTracker = new KeyboardFocusTracker(QCoreApplication::instance());
    QCoreApplication::instance()->installEventFilter(state.focusTracker);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, QCoreApplication::instance(),
        [](Qt::ColorScheme) { ScheduleRecompute(); });
#elif defined(Q_OS_WIN)
    // No colour-scheme signal before Qt 6.5: poll the app-mode value and high contrast instead.
    auto* const poll = new QTimer(QCoreApplication::instance());
    poll->setInterval(3000);
    QObject::connect(poll, &QTimer::timeout, QCoreApplication::instance(), []() { ScheduleRecompute(); });
    poll->start();
#endif
}

double Luminance(const QColor& colour)
{
    const auto channel = [](const double value)
    { return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) + 0.0722 * channel(colour.blueF());
}

// A translucent colour as it shows over an opaque one.
QColor Over(const QColor& top, const QColor& bottom)
{
    const double alpha = top.alphaF();
    return QColor::fromRgbF(static_cast<float>(top.redF() * alpha + bottom.redF() * (1.0 - alpha)),
        static_cast<float>(top.greenF() * alpha + bottom.greenF() * (1.0 - alpha)),
        static_cast<float>(top.blueF() * alpha + bottom.blueF() * (1.0 - alpha)));
}
} // namespace

void Install(QWidget* const root)
{
    State& state = Get();
    InstallOnce();
    std::erase_if(state.roots, [](const QPointer<QWidget>& existing) { return existing.isNull(); });
    // A window that starts with no other window alive reads the preference afresh, as the
    // first one did.
    if (state.roots.empty())
    {
        state.preference = InitialPreference();
        state.preferenceKnown = true;
    }
    const bool added = root != nullptr &&
        std::none_of(state.roots.begin(), state.roots.end(), [root](const QPointer<QWidget>& existing) { return existing == root; });
    if (added)
    {
        state.roots.emplace_back(root);
        root->installEventFilter(state.watcher);
    }
    if (OutOfDate())
        ApplyNow();
    else if (added)
        root->setStyleSheet(StyleSheet());
}

void SetPreference(const Preference preference)
{
    State& state = Get();
    state.preference = preference;
    state.preferenceKnown = true;
    QSettings().setValue(QStringLiteral("appearance/theme"), ToString(preference));
    if (!state.installed)
        return;
    UpdateColorSchemeOverride();
    Recompute();
}

Preference CurrentPreference()
{
    EnsurePreference();
    return Get().preference;
}

Scheme CurrentScheme()
{
    State& state = Get();
    if (state.applied)
        return state.scheme;
    EnsurePreference();
    return EffectiveScheme(state.preference);
}

Scheme SystemScheme()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::instance() != nullptr)
    {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        if (scheme != Qt::ColorScheme::Unknown)
            return scheme == Qt::ColorScheme::Dark ? Scheme::Dark : Scheme::Light;
    }
#endif
#ifdef Q_OS_WIN
    // The format must be explicit: the tests make INI the default.
    const QSettings personalize(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0 ? Scheme::Dark : Scheme::Light;
#else
    return Scheme::Light;
#endif
}

bool HighContrastActive()
{
    return Get().applied && Get().highContrast;
}

const Tokens& Current()
{
    if (HighContrastActive())
        return Get().contrast;
    return CurrentScheme() == Scheme::Dark ? Dark() : Light();
}

const Tokens& Light()
{
    return Get().light;
}

const Tokens& Dark()
{
    return Get().dark;
}

QColor RoleColor(const Role role)
{
    const Tokens& tokens = Current();
    switch (role)
    {
        case Role::Accent:  return tokens.accent;
        case Role::Warning: return tokens.warning;
        case Role::Danger:  return tokens.danger;
        case Role::Success: return tokens.success;
        case Role::TextSecondary: break;
    }
    return tokens.textSecondary;
}

int AddListener(std::function<void()> listener)
{
    State& state = Get();
    const int id = state.nextListener++;
    state.listeners.emplace(id, std::move(listener));
    return id;
}

void RemoveListener(const int id)
{
    Get().listeners.erase(id);
}

QString StyleSheet()
{
    LoadSheets();
    if (HighContrastActive())
        return Get().structureSheet;
    return StyleSheet(Current());
}

QString StyleSheet(const Tokens& tokens)
{
    LoadSheets();
    const State& state = Get();
    return state.structureSheet + QLatin1Char('\n') + FillTemplate(state.colourSheet, tokens);
}

QString DocumentCss()
{
    const Tokens& tokens = Current();
    const auto c = [](const QColor& colour) { return ColourText(colour); };
    QString css;
    css += QStringLiteral("p, li, td, th { color: %1; }\n").arg(c(tokens.text));
    css += QStringLiteral("a { color: %1; text-decoration: none; }\n").arg(c(tokens.link));
    css += QStringLiteral(".speaker { color: %1; font-size: 13px; font-weight: 600; margin-top: 24px; margin-bottom: 4px; }\n")
               .arg(c(tokens.textSecondary));
    css += QStringLiteral(".assistant { color: %1; font-size: 15px; line-height: 160%; }\n").arg(c(tokens.text));
    css += QStringLiteral(".actions, .actions a { color: %1; font-size: 12px; }\n").arg(c(tokens.textSecondary));
    css += QStringLiteral(".userBubble, .userBubble td { color: %1; background-color: %2; font-size: 15px; }\n")
               .arg(c(tokens.text), c(tokens.userBubble));
    css += QStringLiteral(".work, .thought, .work a, .thought a { color: %1; font-size: 13px; }\n").arg(c(tokens.textTertiary));
    css += QStringLiteral(".evidence, .evidence td { color: %1; background-color: %2; font-size: 13px; }\n")
               .arg(c(tokens.textSecondary), c(tokens.surfaceAlt));
    // Qt's rich-text CSS takes whole pixels only and silently drops 12.5px, so memory and
    // proposal rows are 12px (spec 2.6 asks for 12.5).
    css += QStringLiteral(".activityRow { color: %1; font-size: 12px; }\n").arg(c(tokens.textTertiary));
    css += QStringLiteral(".activityRow.warn { color: %1; }\n").arg(c(tokens.warning));
    css += QStringLiteral(".activityRow.error { color: %1; font-size: 13px; }\n").arg(c(tokens.danger));
    css += QStringLiteral(".activityRow.error a { color: %1; }\n").arg(c(tokens.danger));
    css += QStringLiteral(".attachment, .archivedNote { color: %1; font-size: 12px; }\n").arg(c(tokens.textSecondary));
    css += QStringLiteral(".emptyState { color: %1; font-size: 22px; font-weight: 600; }\n").arg(c(tokens.text));
    css += QStringLiteral(".secondary { color: %1; font-size: 13px; font-weight: 400; }\n").arg(c(tokens.textSecondary));
    css += QStringLiteral(".replying { color: %1; }\n").arg(c(tokens.textTertiary));
    css += QStringLiteral("code, pre { font-family: Consolas, 'Cascadia Mono', 'Courier New', monospace; background-color: %1; }\n")
               .arg(c(tokens.codeBackground));
    return css;
}

QIcon Icon(const char* const name, const int logicalSize)
{
    if (name == nullptr || logicalSize <= 0)
        return {};
    const QString full = QString::fromLatin1(name);
    const qsizetype colon = full.indexOf(QLatin1Char(':'));
    const QString base = colon < 0 ? full : full.left(colon);
    const QString tone = colon < 0 ? QString() : full.mid(colon + 1);
    const IconShape* const shape = FindShape(base);
    if (shape == nullptr)
        return {};

    State& state = Get();
    const QString key = QStringLiteral("%1/%2/%3").arg(full).arg(logicalSize).arg(HighContrastActive() ? 2 : int(CurrentScheme()));
    if (const auto cached = state.icons.find(key); cached != state.icons.end())
        return cached->second;

    const Tokens& tokens = Current();
    QColor fixed;
    if (tone == QLatin1String("accent"))
        fixed = tokens.accent;
    else if (tone == QLatin1String("danger"))
        fixed = tokens.danger;
    else if (tone == QLatin1String("warning"))
        fixed = tokens.warning;
    else if (tone == QLatin1String("success"))
        fixed = tokens.success;
    else if (tone == QLatin1String("onAccent"))
        fixed = tokens.textOnAccent;
    else if (tone == QLatin1String("text"))
        fixed = tokens.text;
    else if (!tone.isEmpty() && tone != QLatin1String("secondary"))
        return {};

    struct Variant
    {
        QIcon::Mode mode;
        QIcon::State state;
        QColor colour;
    };
    const QColor normal = fixed.isValid() ? fixed : tokens.textSecondary;
    const QColor checked = fixed.isValid() ? fixed : tokens.accent;
    const QColor hover = fixed.isValid() ? fixed : tokens.text;
    const Variant variants[] = {
        {QIcon::Normal, QIcon::Off, normal},
        {QIcon::Active, QIcon::Off, hover},
        {QIcon::Selected, QIcon::Off, fixed.isValid() ? fixed : tokens.selectionText},
        {QIcon::Disabled, QIcon::Off, tokens.textDisabled},
        {QIcon::Normal, QIcon::On, checked},
        {QIcon::Active, QIcon::On, checked},
        {QIcon::Selected, QIcon::On, fixed.isValid() ? fixed : tokens.selectionText},
        {QIcon::Disabled, QIcon::On, tokens.textDisabled},
    };
    QIcon icon;
    const QString elements = QString::fromLatin1(shape->elements);
    for (const Variant& variant : variants)
        for (const qreal ratio : {1.0, 2.0, 3.0})
            icon.addPixmap(RenderSvg(elements, variant.colour, logicalSize, ratio), variant.mode, variant.state);
    state.icons.emplace(key, icon);
    return icon;
}

std::vector<const char*> IconNames()
{
    std::vector<const char*> names;
    for (const IconShape& shape : IconShapes)
        names.push_back(shape.name);
    return names;
}

void BindIcon(QAbstractButton* const button, const char* const name, const int size)
{
    if (button == nullptr || name == nullptr)
        return;
    State& state = Get();
    std::erase_if(state.buttons,
        [button](const ButtonBinding& binding) { return binding.button.isNull() || binding.button == button; });
    state.buttons.push_back({button, name, size});
    button->setIcon(Icon(name, size));
    button->setIconSize(QSize(size, size));
}

void BindIcon(QAction* const action, const char* const name)
{
    if (action == nullptr || name == nullptr)
        return;
    State& state = Get();
    std::erase_if(state.actions,
        [action](const ActionBinding& binding) { return binding.action.isNull() || binding.action == action; });
    state.actions.push_back({action, name});
    action->setIcon(Icon(name, 16));
}

void BindSvgLabel(QLabel* const label, const QString& svgPaths, const Role role, const int size)
{
    if (label == nullptr)
        return;
    State& state = Get();
    std::erase_if(state.labels, [label](const LabelBinding& binding) { return binding.label.isNull() || binding.label == label; });
    state.labels.push_back({label, svgPaths, role, size});
    const qreal ratio = std::max<qreal>(label->devicePixelRatioF(), 2.0);
    label->setPixmap(RenderSvg(svgPaths, RoleColor(role), size, ratio));
}

bool KeyboardFocusAfter(const bool before, const QFocusEvent& event)
{
    switch (event.reason())
    {
        case Qt::TabFocusReason:
        case Qt::BacktabFocusReason:
        case Qt::ShortcutFocusReason: return event.gotFocus();
        case Qt::ActiveWindowFocusReason:
        case Qt::PopupFocusReason:
        case Qt::MenuBarFocusReason: return before;
        case Qt::MouseFocusReason: return false;
        case Qt::OtherFocusReason:
        case Qt::NoFocusReason: break;
    }
    return event.gotFocus() && before;
}

void PolishMenu(QMenu* const menu)
{
    if (menu == nullptr || menu->property("reviaMenuPolished").toBool())
        return;
    menu->setProperty("reviaMenuPolished", true);
#ifdef Q_OS_WIN
    // Windows 11 rounds the menu and gives it the system shadow; older systems ignore the
    // attribute and keep a square opaque menu.
    QObject::connect(menu, &QMenu::aboutToShow, menu,
        [menu]()
        {
            const SetWindowAttributeFunction setter = WindowAttributeSetter();
            if (setter == nullptr || HighContrastActive())
                return;
            const DWORD roundSmall = 3; // DWMWCP_ROUNDSMALL
            setter(reinterpret_cast<HWND>(menu->winId()), 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &roundSmall, sizeof(roundSmall));
        });
#endif
}

void ApplyNativeFrame(QWidget* const topLevel)
{
#ifdef Q_OS_WIN
    if (topLevel == nullptr || !topLevel->isWindow())
        return;
    const SetWindowAttributeFunction setter = WindowAttributeSetter();
    if (setter == nullptr)
        return;
    const HWND window = reinterpret_cast<HWND>(topLevel->winId());
    if (HighContrastActive())
    {
        // The system's contrast colours, not the ones set before high contrast came on.
        const COLORREF systemDefault = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
        for (const DWORD attribute : {DWORD(35), DWORD(36), DWORD(34)})
            setter(window, attribute, &systemDefault, sizeof(systemDefault));
        return;
    }
    const BOOL dark = CurrentScheme() == Scheme::Dark ? TRUE : FALSE;
    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE; Windows 10 builds before 20H1 used 19.
    if (FAILED(setter(window, 20, &dark, sizeof(dark))))
        setter(window, 19, &dark, sizeof(dark));
    // Windows 11 only; earlier versions refuse these and keep their own caption colours.
    const Tokens& tokens = Current();
    const COLORREF caption = ColourRef(tokens.background);
    const COLORREF captionText = ColourRef(tokens.text);
    const COLORREF border = ColourRef(tokens.border);
    setter(window, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    setter(window, 36 /* DWMWA_TEXT_COLOR */, &captionText, sizeof(captionText));
    setter(window, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
#else
    Q_UNUSED(topLevel);
#endif
}

double ContrastRatio(const QColor& first, const QColor& second)
{
    const double a = Luminance(first);
    const double b = Luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

std::vector<ContrastPair> ContrastPairs(const Tokens& t)
{
    constexpr double Text = 4.5;
    constexpr double Boundary = 3.0;
    std::vector<ContrastPair> pairs;
    const auto add = [&pairs](const char* name, const QColor& fg, const QColor& bg, const double minimum)
    { pairs.push_back({name, fg, bg, minimum}); };

    // Primary text everywhere it can sit, including hover, selected and every soft fill.
    add("text on background", t.text, t.background, Text);
    add("text on sidebar", t.text, t.sidebar, Text);
    add("text on sidebarHover", t.text, t.sidebarHover, Text);
    add("text on sidebarSelected", t.text, t.sidebarSelected, Text);
    add("text on surface", t.text, t.surface, Text);
    add("text on surfaceAlt", t.text, t.surfaceAlt, Text);
    add("text on surfaceHover", t.text, t.surfaceHover, Text);
    add("text on inputBackground", t.text, t.inputBackground, Text);
    add("text on userBubble", t.text, t.userBubble, Text);
    add("text on codeBackground", t.text, t.codeBackground, Text);
    add("text on accentSoft (highlighted row, banners)", t.text, t.accentSoft, Text);
    add("text on dangerSoft (stop bar)", t.text, t.dangerSoft, Text);
    add("text on warningSoft (restart banner, archived chat)", t.text, t.warningSoft, Text);
    add("text on successSoft", t.text, t.successSoft, Text);
    add("selectionText on selection", t.selectionText, t.selection, Text);

    // Secondary text: descriptions, metadata, subtitles on selected rows, placeholders.
    add("textSecondary on background", t.textSecondary, t.background, Text);
    add("textSecondary on sidebar", t.textSecondary, t.sidebar, Text);
    add("textSecondary on sidebarHover", t.textSecondary, t.sidebarHover, Text);
    add("textSecondary on sidebarSelected", t.textSecondary, t.sidebarSelected, Text);
    add("textSecondary on surface", t.textSecondary, t.surface, Text);
    add("textSecondary on surfaceAlt (neutral badge)", t.textSecondary, t.surfaceAlt, Text);
    add("textSecondary on surfaceHover", t.textSecondary, t.surfaceHover, Text);
    add("textSecondary on selection (palette subtitle)", t.textSecondary, t.selection, Text);
    add("textSecondary on userBubble", t.textSecondary, t.userBubble, Text);
    add("textSecondary on accentSoft (banners)", t.textSecondary, t.accentSoft, Text);
    add("textSecondary on warningSoft (quiet button in a banner)", t.textSecondary, t.warningSoft, Text);
    add("placeholder on inputBackground", t.textSecondary, t.inputBackground, Text);

    // Tertiary text: captions, hints and working rows; never on a selected row.
    add("textTertiary on background", t.textTertiary, t.background, Text);
    add("textTertiary on sidebar", t.textTertiary, t.sidebar, Text);
    add("textTertiary on sidebarHover", t.textTertiary, t.sidebarHover, Text);
    add("textTertiary on surface", t.textTertiary, t.surface, Text);
    add("textTertiary on surfaceAlt", t.textTertiary, t.surfaceAlt, Text);
    add("textTertiary on surfaceHover", t.textTertiary, t.surfaceHover, Text);
    add("textTertiary on userBubble", t.textTertiary, t.userBubble, Text);
    add("textTertiary on codeBackground", t.textTertiary, t.codeBackground, Text);

    // Accent and link text, and accent badges.
    add("accent on background", t.accent, t.background, Text);
    add("accent on sidebar", t.accent, t.sidebar, Text);
    add("accent on sidebarHover", t.accent, t.sidebarHover, Text);
    add("accent on surface", t.accent, t.surface, Text);
    add("accent on surfaceAlt", t.accent, t.surfaceAlt, Text);
    add("accent on surfaceHover", t.accent, t.surfaceHover, Text);
    add("accent on accentSoft (privacy and autonomy badges, chips)", t.accent, t.accentSoft, Text);
    add("link on background", t.link, t.background, Text);
    add("link on surface", t.link, t.surface, Text);
    add("link on surfaceAlt (evidence)", t.link, t.surfaceAlt, Text);
    add("link on userBubble", t.link, t.userBubble, Text);

    // Text and icons on filled buttons.
    add("textOnAccent on accent", t.textOnAccent, t.accent, Text);
    add("textOnAccent on accentHover", t.textOnAccent, t.accentHover, Text);
    add("textOnAccent on accentPressed", t.textOnAccent, t.accentPressed, Text);
    add("textOnAccent on danger (desktop stop)", t.textOnAccent, t.danger, Text);

    // Status colours as text: only on background, surface, sidebar or their own soft colour.
    add("danger on background", t.danger, t.background, Text);
    add("danger on surface", t.danger, t.surface, Text);
    add("danger on sidebar", t.danger, t.sidebar, Text);
    add("danger on dangerSoft (risky badge, attachment remove on hover)", t.danger, t.dangerSoft, Text);
    add("danger on surfaceHover (menu item, hovered or highlighted setting row)", t.danger, t.surfaceHover, Text);
    add("warning on background", t.warning, t.background, Text);
    add("warning on surface", t.warning, t.surface, Text);
    add("warning on sidebar", t.warning, t.sidebar, Text);
    add("warning on warningSoft (restart badge)", t.warning, t.warningSoft, Text);
    add("warning on surfaceHover (hovered or highlighted setting row)", t.warning, t.surfaceHover, Text);
    add("success on background", t.success, t.background, Text);
    add("success on surface", t.success, t.surface, Text);
    add("success on sidebar", t.success, t.sidebar, Text);
    add("success on successSoft", t.success, t.successSoft, Text);
    add("success on surfaceHover (hovered or highlighted setting row)", t.success, t.surfaceHover, Text);

    // The centred percentage of a progress bar sits on the bar and on its translucent fill;
    // reviaTheme.qss draws every ::chunk at this opacity over inputBackground.
    const auto fill = [&t](const QColor& colour)
    {
        QColor top = colour;
        top.setAlphaF(ProgressFillPercent / 100.0);
        return Over(top, t.inputBackground);
    };
    add("text on progress fill (accent, elevated)", t.text, fill(t.accent), Text);
    add("text on progress fill (success, idle)", t.text, fill(t.success), Text);
    add("text on progress fill (warning, high)", t.text, fill(t.warning), Text);
    add("text on progress fill (danger, critical or over budget)", t.text, fill(t.danger), Text);

    // Inverted and overlaid text.
    add("toast text", t.background, t.text, Text);
    add("lock overlay text on scrim over background", t.text, Over(t.scrim, t.background), Text);

    // Boundaries and glyphs that identify a control (WCAG 1.4.11).
    add("inputBorder on background", t.inputBorder, t.background, Boundary);
    add("inputBorder on sidebar", t.inputBorder, t.sidebar, Boundary);
    add("inputBorder on surface", t.inputBorder, t.surface, Boundary);
    add("switchOff on background", t.switchOff, t.background, Boundary);
    add("switchOff on surface", t.switchOff, t.surface, Boundary);
    add("focusRing on background", t.focusRing, t.background, Boundary);
    add("focusRing on surface", t.focusRing, t.surface, Boundary);
    add("focusRing on sidebar", t.focusRing, t.sidebar, Boundary);
    add("accent switch track on surface", t.accent, t.surface, Boundary);
    add("accent icon on sidebarSelected (checked tool button)", t.accent, t.sidebarSelected, Boundary);
    add("background inset ring on accent", t.background, t.accent, Boundary);
    add("background inset ring on danger", t.background, t.danger, Boundary);
    return pairs;
}

QString ToString(const Preference preference)
{
    switch (preference)
    {
        case Preference::Light: return QStringLiteral("light");
        case Preference::Dark:  return QStringLiteral("dark");
        case Preference::System: break;
    }
    return QStringLiteral("system");
}

Preference PreferenceFromString(const QString& text)
{
    const QString value = text.trimmed().toLower();
    if (value == QLatin1String("light"))
        return Preference::Light;
    if (value == QLatin1String("dark"))
        return Preference::Dark;
    return Preference::System;
}

void SetHighContrastForTest(const std::optional<bool> active)
{
    State& state = Get();
    state.highContrastOverride = active;
    if (state.installed)
        Recompute();
}
}
