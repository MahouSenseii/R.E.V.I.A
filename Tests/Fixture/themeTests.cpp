// Revia.Theme: the light and dark tokens, the stylesheet built from them and the widgets that
// paint from them. Runs as `ReviaDesktopStopTests --suite=theme`, inside main()'s QApplication
// with temporary QSettings. REVIA_UI_RENDER_DIR=<dir> also saves a gallery of common widgets in
// each scheme, for review.
#include "desktopSuites.h"
#include "desktopObjectNames.h"
#include "chatMarkdown.h"
#include "internetActivityPanel.h"
#include "pipelinePanel.h"
#include "theme.h"
#include "toggleSwitch.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFocusEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace
{
namespace theme = revia::desktop::theme;

struct Results
{
    int failures = 0;
    void Expect(const bool condition, const std::string& message)
    {
        if (condition)
            return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
};

void Settle()
{
    for (int pass = 0; pass < 4; ++pass)
    {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

bool Near(const QColor& first, const QColor& second, const int tolerance = 4)
{
    return std::abs(first.red() - second.red()) <= tolerance && std::abs(first.green() - second.green()) <= tolerance &&
        std::abs(first.blue() - second.blue()) <= tolerance;
}

std::string Describe(const QColor& colour)
{
    return colour.name(QColor::HexRgb).toStdString();
}

QColor PixelAt(QWidget* widget, const QPoint& point)
{
    const QImage image = widget->grab().toImage();
    const qreal ratio = image.devicePixelRatio();
    return image.pixelColor(QPoint(qRound(point.x() * ratio), qRound(point.y() * ratio)));
}

// The most opaque pixel of an icon: its stroke colour without antialiasing blend.
QColor StrokeColour(const QPixmap& pixmap)
{
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    QColor best(Qt::transparent);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
        {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > best.alpha())
                best = pixel;
        }
    return best;
}

// QAbstractSpinBox::initStyleOption is protected; this lets a test ask the style where the up
// button sits. No signals or slots, so no moc.
class InspectableSpinBox final : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

    [[nodiscard]] QRect UpButton() const
    {
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        return style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, this);
    }
};

QString StructureSheet()
{
    QFile file(QStringLiteral(":/revia/revia.qss"));
    return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll()) : QString();
}

QString ColourTemplate()
{
    QFile file(QStringLiteral(":/revia/reviaTheme.qss"));
    return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll()) : QString();
}

QString ContrastSheet()
{
    QFile file(QStringLiteral(":/revia/reviaContrast.qss"));
    return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll()) : QString();
}

void CheckContrast(Results& results, const theme::Tokens& tokens, const char* scheme)
{
    const std::vector<theme::ContrastPair> pairs = theme::ContrastPairs(tokens);
    results.Expect(pairs.size() >= 60, std::string(scheme) + ": ContrastPairs lists too few pairs");
    for (const theme::ContrastPair& pair : pairs)
    {
        const double ratio = theme::ContrastRatio(pair.fg, pair.bg);
        results.Expect(ratio + 1e-9 >= pair.minimum, std::string(scheme) + ": " + pair.name + " is " + std::to_string(ratio) +
            ":1 (" + Describe(pair.fg) + " on " + Describe(pair.bg) + "), needs " + std::to_string(pair.minimum));
    }
}

void CheckSheet(Results& results, const QString& sheet, const char* scheme)
{
    results.Expect(!sheet.contains(QLatin1String("{{")), std::string(scheme) + ": StyleSheet() still holds a {{token}} placeholder");
    for (const revia::desktop::names::Name& name : revia::desktop::names::kStyled)
    {
        const QString id = QString::fromUtf8(name.name.data(), static_cast<qsizetype>(name.name.size()));
        const QRegularExpression rule(QStringLiteral("#%1(?![A-Za-z0-9_])").arg(QRegularExpression::escape(id)));
        results.Expect(sheet.contains(rule), std::string(scheme) + ": no #" + id.toStdString() + " rule in the stylesheet");
    }
    for (const std::string_view role : revia::desktop::names::kStyledRoles)
        results.Expect(sheet.contains(QStringLiteral("[reviaRole=\"%1\"]").arg(QString::fromUtf8(role.data(), static_cast<qsizetype>(role.size())))),
            std::string(scheme) + ": no [reviaRole=\"" + std::string(role) + "\"] rule");
    for (const std::string_view style : revia::desktop::names::kStyledClasses)
        results.Expect(sheet.contains(QStringLiteral("[reviaClass=\"%1\"]").arg(QString::fromUtf8(style.data(), static_cast<qsizetype>(style.size())))),
            std::string(scheme) + ": no [reviaClass=\"" + std::string(style) + "\"] rule");
}

// Builds a page of common controls in the current scheme and saves it, for a human to look at.
void SaveGallery(QWidget* root, const QString& directory, const QString& name)
{
    // A window of its own, parented to the root so it inherits the root's stylesheet.
    auto* gallery = new QWidget(root, Qt::Window);
    gallery->setObjectName("rootPanel");
    gallery->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QGridLayout(gallery);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setHorizontalSpacing(16);
    layout->setVerticalSpacing(12);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(new QPushButton("Secondary", gallery));
    auto* primary = new QPushButton("Primary", gallery);
    primary->setProperty("primary", true);
    buttons->addWidget(primary);
    auto* destructive = new QPushButton("Delete", gallery);
    destructive->setObjectName("destructiveButton");
    buttons->addWidget(destructive);
    auto* disabled = new QPushButton("Disabled", gallery);
    disabled->setEnabled(false);
    buttons->addWidget(disabled);
    auto* tool = new QToolButton(gallery);
    theme::BindIcon(tool, "settings");
    tool->setToolTip("Settings");
    buttons->addWidget(tool);
    auto* checkedTool = new QToolButton(gallery);
    checkedTool->setCheckable(true);
    checkedTool->setChecked(true);
    theme::BindIcon(checkedTool, "mic");
    buttons->addWidget(checkedTool);
    buttons->addStretch(1);
    layout->addLayout(buttons, 0, 0, 1, 2);

    auto* inputs = new QHBoxLayout();
    auto* line = new QLineEdit(gallery);
    line->setPlaceholderText("Search settings");
    inputs->addWidget(line);
    auto* combo = new QComboBox(gallery);
    combo->addItems({"Only when I ask", "Hard questions", "Never"});
    inputs->addWidget(combo);
    auto* spin = new QSpinBox(gallery);
    spin->setValue(12);
    inputs->addWidget(spin);
    layout->addLayout(inputs, 1, 0, 1, 2);

    auto* group = new QGroupBox("Spoken replies", gallery);
    auto* groupLayout = new QVBoxLayout(group);
    for (const auto& [label, description, badge, on] :
        {std::tuple{"Speak replies", "Read each reply aloud.", "", true},
            std::tuple{"Use the microphone", "Listen for speech when you press the mic.", "privacy", false},
            std::tuple{"Trust the local API as me", "Any program with the install token speaks as you.", "danger", true}})
    {
        auto* row = new QFrame(group);
        row->setProperty("reviaClass", "settingRow");
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(16, 12, 16, 12);
        auto* text = new QVBoxLayout();
        auto* title = new QLabel(label, row);
        title->setObjectName("settingTitle");
        text->addWidget(title);
        auto* detail = new QLabel(description, row);
        detail->setObjectName("settingDescription");
        text->addWidget(detail);
        if (std::string(badge).size() > 0)
        {
            auto* chip = new QLabel(std::string(badge) == "danger" ? "Risky" : "Privacy", row);
            chip->setObjectName("settingBadge");
            chip->setProperty("kind", badge);
            text->addWidget(chip, 0, Qt::AlignLeft);
        }
        rowLayout->addLayout(text, 1);
        auto* toggle = new ToggleSwitch(ToggleSwitch::Accent::Cyan, row);
        toggle->setChecked(on);
        rowLayout->addWidget(toggle);
        groupLayout->addWidget(row);
    }
    {
        // FocusSetting's flash on a risky row, whose warning must stay readable through it.
        auto* row = new QFrame(group);
        row->setProperty("reviaClass", "settingRow");
        row->setProperty("highlight", true);
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(16, 12, 16, 12);
        auto* title = new QLabel("Let Revia use the desktop", row);
        title->setObjectName("settingTitle");
        rowLayout->addWidget(title);
        auto* warning = new QLabel("Revia can click and type in any window while this is on.", row);
        warning->setObjectName("settingWarning");
        rowLayout->addWidget(warning);
        groupLayout->addWidget(row);
    }
    auto* check = new QCheckBox("Follow new activity", group);
    check->setChecked(true);
    groupLayout->addWidget(check);
    groupLayout->addWidget(new QCheckBox("Show work summaries", group));
    layout->addWidget(group, 2, 0);

    auto* tabs = new QTabWidget(gallery);
    auto* pipelines = new PipelinePanel(tabs);
    revia::runtime::RuntimeEvent event;
    event.kind = revia::runtime::RuntimeEventKind::ComponentStatus;
    for (const auto& [component, phase] : {std::pair{"Voice", "Speaking"}, std::pair{"Memory", "Ready"},
             std::pair{"Vision", "Degraded"}, std::pair{"Internet", "Error"}})
    {
        event.component = component;
        event.phase = phase;
        event.message = std::string(phase) + " for the gallery";
        pipelines->Observe(event);
    }
    tabs->addTab(pipelines, "Pipelines");
    tabs->addTab(new QWidget(tabs), "Resources");
    layout->addWidget(tabs, 2, 1);

    auto* transcript = new QTextBrowser(gallery);
    transcript->setObjectName("chatHistory");
    transcript->document()->setDefaultStyleSheet(theme::DocumentCss());
    transcript->setHtml(QStringLiteral("<p class='speaker'>Revia</p><div class='assistant'>%1</div>"
                                       "<p class='actions'><a href='copy:1'>Copy</a> · <a href='regen:1'>Regenerate</a></p>"
                                       "<p class='activityRow'>▸ Remembered that you prefer CMake presets</p>"
                                       "<p class='activityRow error'>⚠ Couldn't finish this reply: the model stopped · <a href='retry:'>Retry</a></p>")
                            .arg(revia::desktop::RenderUntrustedMarkdown(
                                "Run `cmake --preset debug`, then see [the docs](https://example.com).\n\n> Quoted note\n\n```\ncmake --build build\n```")));
    layout->addWidget(transcript, 3, 0);

    auto* side = new QVBoxLayout();
    auto* list = new QListWidget(gallery);
    list->addItems({"Project planning", "Build failures", "Holiday ideas"});
    list->setCurrentRow(1);
    side->addWidget(list);
    for (const char* pressure : {"Idle", "Elevated", "High", "Critical"})
    {
        auto* bar = new QProgressBar(gallery);
        bar->setValue(pressure[0] == 'I' ? 25 : pressure[0] == 'E' ? 50 : pressure[0] == 'H' ? 75 : 95);
        bar->setProperty("pressure", pressure);
        side->addWidget(bar);
    }
    layout->addLayout(side, 3, 1);

    gallery->resize(980, 840);
    gallery->show();
    Settle();
    gallery->grab().save(QDir(directory).filePath(name));
    delete gallery;
    Settle();
}

int RunThemeSuite(int, char**)
{
    Results results;
    const QByteArray originalTheme = qgetenv("REVIA_THEME");
    const QString renderDirectory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");

    // Tokens and the stylesheet built from them.
    CheckContrast(results, theme::Light(), "light");
    CheckContrast(results, theme::Dark(), "dark");
    results.Expect(theme::Light().inputBorder == QColor("#8C877D") && theme::Dark().accent == QColor("#4FD1C5"),
        "token values drifted from the design spec");
    results.Expect(!theme::StyleSheet(theme::Light()).contains(QLatin1String("{{")), "light stylesheet keeps a placeholder");
    CheckSheet(results, theme::StyleSheet(theme::Light()), "light");
    CheckSheet(results, theme::StyleSheet(theme::Dark()), "dark");
    results.Expect(theme::StyleSheet(theme::Light()).contains(QLatin1String("#faf9f7")) &&
            theme::StyleSheet(theme::Dark()).contains(QLatin1String("#1f1e1c")),
        "the colour template was not filled from the scheme's tokens");
    const QString structure = StructureSheet();
    results.Expect(!structure.isEmpty() && !ColourTemplate().isEmpty(), "revia.qss or reviaTheme.qss is missing from the resources");
    {
        // ContrastPairs checks the centred percentage over each progress fill at one opacity, so
        // every fill in the template must use it.
        const QRegularExpression chunkFill(QStringLiteral(R"(::chunk\s*\{[^}]*\{\{\s*\w+(?:/(\d+))?\s*\}\})"));
        int fills = 0;
        for (auto matches = chunkFill.globalMatch(ColourTemplate()); matches.hasNext();)
        {
            const QRegularExpressionMatch fill = matches.next();
            ++fills;
            results.Expect(fill.captured(1).toInt() == theme::ProgressFillPercent,
                "a progress fill is not drawn at the opacity ContrastPairs checks: " + fill.captured(0).simplified().toStdString());
        }
        results.Expect(fills >= 5, "reviaTheme.qss lost its progress-bar fills");
    }
    const QRegularExpression colourLiteral(QStringLiteral(R"(#[0-9A-Fa-f]{6}(?![\w-])|#[0-9A-Fa-f]{3}(?![\w-])|rgba?\()"));
    results.Expect(!structure.contains(colourLiteral), "revia.qss must hold structure only, but it names a colour: " +
        colourLiteral.match(structure).captured(0).toStdString());
    results.Expect(!structure.contains(QLatin1String("url(")), "revia.qss loads an image, which bakes in a colour high contrast cannot change");
    const QString contrastSheet = ContrastSheet();
    results.Expect(!contrastSheet.isEmpty() && !contrastSheet.contains(colourLiteral) && !contrastSheet.contains(QLatin1String("{{")) &&
            !contrastSheet.contains(QLatin1String("url(")),
        "reviaContrast.qss is missing, or names a colour of its own instead of the system palette's");
    results.Expect(theme::PreferenceFromString("dark") == theme::Preference::Dark &&
            theme::PreferenceFromString(" Light ") == theme::Preference::Light &&
            theme::PreferenceFromString("sepia") == theme::Preference::System &&
            theme::ToString(theme::Preference::System) == QLatin1String("system") &&
            theme::ToString(theme::PreferenceFromString(theme::ToString(theme::Preference::Dark))) == QLatin1String("dark"),
        "preference names do not round-trip");

    // REVIA_THEME wins over the stored preference when the first window installs the theme.
    QSettings().setValue("appearance/theme", "dark");
    qputenv("REVIA_THEME", "light");
    auto root = std::make_unique<QWidget>();
    root->setObjectName("themeTestRoot");
    auto* rootLayout = new QVBoxLayout(root.get());
    auto* sample = new QPushButton(root.get());
    sample->setObjectName("sampleButton");
    sample->setFixedSize(80, 32);
    rootLayout->addWidget(sample);
    auto* onSwitch = new ToggleSwitch(ToggleSwitch::Accent::Red, root.get());
    onSwitch->setChecked(true);
    rootLayout->addWidget(onSwitch);
    auto* offSwitch = new ToggleSwitch(ToggleSwitch::Accent::Cyan, root.get());
    rootLayout->addWidget(offSwitch);
    auto* iconButton = new QToolButton(root.get());
    rootLayout->addWidget(iconButton);
    auto* svgLabel = new QLabel(root.get());
    rootLayout->addWidget(svgLabel);
    auto* pipelines = new PipelinePanel(root.get());
    rootLayout->addWidget(pipelines);
    auto* internet = new InternetActivityPanel(root.get());
    rootLayout->addWidget(internet);
    auto* focusList = new QListWidget(root.get());
    focusList->addItems({"Project planning", "Build failures"});
    focusList->setCurrentRow(0);
    rootLayout->addWidget(focusList);
    auto* focusTabs = new QTabBar(root.get());
    focusTabs->addTab("Pipelines");
    focusTabs->addTab("Resources");
    rootLayout->addWidget(focusTabs);
    auto* focusCheck = new QCheckBox("Follow new activity", root.get());
    focusCheck->setChecked(true);
    rootLayout->addWidget(focusCheck);
    root->resize(720, 900);
    theme::Install(root.get());
    root->show();
    Settle();

    results.Expect(theme::CurrentPreference() == theme::Preference::Light && theme::CurrentScheme() == theme::Scheme::Light,
        "REVIA_THEME=light did not win over appearance/theme=dark");
    results.Expect(QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0,
        "Install did not select the Fusion style");
    results.Expect(QApplication::font().families().contains(QStringLiteral("Segoe UI Variable Text")),
        "Install did not set the app font families");
    results.Expect(root->styleSheet() == theme::StyleSheet(), "the root does not carry the generated stylesheet");
    results.Expect(QApplication::palette().color(QPalette::Window) == theme::Light().background &&
            QApplication::palette().color(QPalette::PlaceholderText) == theme::Light().textSecondary &&
            QApplication::palette().color(QPalette::Highlight) == theme::Light().selection,
        "the light palette was not applied");
    results.Expect(!theme::HighContrastActive(), "high contrast reported active without the system or a test asking");

    // Toggle switches paint from the tokens: accent track when on, switchOff outline and knob when off.
    results.Expect(onSwitch->sizeHint() == QSize(48, 30), "ToggleSwitch is not a 40x22 track with room for its focus ring");
    const QPoint trackLeft(4 + 6, onSwitch->height() / 2);
    const QPoint offKnob(4 + 5 + 6, offSwitch->height() / 2);
    results.Expect(Near(PixelAt(onSwitch, trackLeft), theme::Light().accent),
        "a light 'on' switch is not the accent colour: " + Describe(PixelAt(onSwitch, trackLeft)));
    results.Expect(Near(PixelAt(offSwitch, offKnob), theme::Light().switchOff),
        "a light 'off' switch knob is not switchOff: " + Describe(PixelAt(offSwitch, offKnob)));
    {
        // The whole switch is the hit target, not only QCheckBox's indicator box at the left.
        const QPointF far(offSwitch->width() - 6, offSwitch->height() / 2.0);
        const QPointF global = offSwitch->mapToGlobal(far);
        QMouseEvent press(QEvent::MouseButtonPress, far, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, far, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(offSwitch, &press);
        QApplication::sendEvent(offSwitch, &release);
        results.Expect(offSwitch->isChecked(), "clicking the right end of a switch did not toggle it");
        offSwitch->setChecked(false);
    }

    // Keyboard focus rings. Every focusable control shows one; an item view takes focus from a
    // click as well, so its ring follows the keyboard only, as a switch's does. A ring survives
    // the window losing and regaining activation, and never moves what is inside the control.
    root->activateWindow();
    Settle();
    results.Expect(root->isActiveWindow(), "the theme test window could not be activated");
    {
        const auto reactivate = [](QWidget* widget)
        {
            QFocusEvent out(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
            QApplication::sendEvent(widget, &out);
            QFocusEvent in(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
            QApplication::sendEvent(widget, &in);
            Settle();
        };
        const QRect listViewport = focusList->viewport()->geometry();
        const QRect firstTab = focusTabs->tabRect(0);
        struct FocusCase
        {
            QWidget* control;
            std::string name;
            bool clickShowsRing;
        };
        const FocusCase cases[] = {{focusList, "a list", false}, {focusTabs, "a tab bar", true},
            {focusCheck, "a check box", true}, {offSwitch, "a switch", false}};
        for (const auto& [control, name, clickShowsRing] : cases)
        {
            sample->setFocus(Qt::OtherFocusReason);
            Settle();
            const QImage rest = control->grab().toImage();
            control->setFocus(Qt::MouseFocusReason);
            Settle();
            const QImage clicked = control->grab().toImage();
            if (!clickShowsRing)
                results.Expect(clicked == rest, name + " shows a focus ring after a click");
            sample->setFocus(Qt::OtherFocusReason);
            control->setFocus(Qt::TabFocusReason);
            Settle();
            const QImage keyed = control->grab().toImage();
            results.Expect(control->hasFocus() && keyed != rest, name + " looks the same with keyboard focus");
            reactivate(control);
            results.Expect(control->grab().toImage() == keyed, name + " lost its focus ring when the window was reactivated");
        }
        focusList->setFocus(Qt::TabFocusReason);
        Settle();
        results.Expect(Near(PixelAt(focusList, QPoint(1, focusList->height() / 2)), theme::Light().focusRing),
            "a keyboard-focused list's ring is not focusRing: " + Describe(PixelAt(focusList, QPoint(1, focusList->height() / 2))));
        results.Expect(focusList->viewport()->geometry() == listViewport && focusTabs->tabRect(0) == firstTab,
            "a focus ring moved a list's rows or resized a tab");
        sample->setFocus(Qt::OtherFocusReason);
        Settle();
    }

    // Bound icons and labels follow the scheme.
    theme::BindIcon(iconButton, "mic");
    theme::BindSvgLabel(svgLabel, QStringLiteral("<circle cx='12' cy='12' r='9'/>"), theme::Role::Danger, 24);
    auto action = std::make_unique<QAction>("Activity");
    theme::BindIcon(action.get(), "activity");
    results.Expect(!action->icon().isNull(), "BindIcon(QAction*) gave no icon");
    results.Expect(Near(StrokeColour(iconButton->icon().pixmap(18, 18)), theme::Light().textSecondary),
        "a bound light icon is not textSecondary");
    results.Expect(Near(StrokeColour(svgLabel->pixmap().copy()), theme::Light().danger), "a bound light SVG label is not danger");
    results.Expect(Near(StrokeColour(theme::Icon("cursor:danger").pixmap(18, 18)), theme::Light().danger) &&
            Near(StrokeColour(theme::Icon("arrow-up:onAccent").pixmap(18, 18)), theme::Light().textOnAccent),
        "tone suffixes do not pick the tone");
    for (const char* name : theme::IconNames())
        results.Expect(!theme::Icon(name).isNull() && StrokeColour(theme::Icon(name).pixmap(18, 18)).alpha() > 200,
            std::string("icon ") + name + " renders nothing");
    results.Expect(theme::IconNames().size() >= 48, "the icon table lost names the design lists");
    results.Expect(theme::Icon("no-such-icon").isNull() && theme::Icon("mic:sparkly").isNull(), "an unknown icon name gave an icon");

    // Pipeline and internet tables keep a tone and recolour from it.
    QTableWidget* pipelineTable = pipelines->findChild<QTableWidget*>();
    QTableWidget* internetTable = internet->findChild<QTableWidget*>();
    revia::runtime::RuntimeEvent status;
    status.kind = revia::runtime::RuntimeEventKind::ComponentStatus;
    status.component = "Voice";
    status.phase = "Error";
    pipelines->Observe(status);
    status.component = "Internet activity";
    status.phase = "Ready";
    status.initiator = "User";
    status.turnId = 7;
    internet->Observe(status);
    const auto pipelineCell = [pipelineTable](const QString& component) -> QTableWidgetItem*
    {
        for (int row = 0; pipelineTable != nullptr && row < pipelineTable->rowCount(); ++row)
            if (pipelineTable->item(row, 0) != nullptr && pipelineTable->item(row, 0)->text() == component)
                return pipelineTable->item(row, 2);
        return nullptr;
    };
    QTableWidgetItem* voiceState = pipelineCell("Voice");
    QTableWidgetItem* memoryState = pipelineCell("Memory");
    QTableWidgetItem* lookupState = internetTable != nullptr && internetTable->rowCount() > 0 ? internetTable->item(0, 1) : nullptr;
    results.Expect(voiceState != nullptr && memoryState != nullptr && lookupState != nullptr, "the panels' state cells are missing");
    if (voiceState != nullptr && memoryState != nullptr && lookupState != nullptr)
        results.Expect(voiceState->foreground().color() == theme::Light().danger &&
                memoryState->foreground().color() == theme::Light().textSecondary &&
                lookupState->foreground().color() == theme::Light().success,
            "light state cells are not toned danger / textSecondary / success");

    // Markdown and the transcript stylesheet bake the current colours in.
    const QString lightMarkdown = revia::desktop::RenderUntrustedMarkdown("See [docs](https://example.com) and `code`.");
    results.Expect(lightMarkdown.contains(theme::Light().link.name()) && lightMarkdown.contains(theme::Light().codeBackground.name()),
        "Markdown links and inline code do not use the light link and code colours");
    results.Expect(lightMarkdown.contains(QLatin1String("Consolas")), "inline code lost its monospace face");
    results.Expect(theme::DocumentCss().contains(theme::Light().text.name()), "DocumentCss does not carry the light text colour");
    {
        // Qt's rich-text CSS drops what it cannot parse without a word (a 12.5px size, for one),
        // so check what a document does with each class rather than the text of the sheet.
        QTextDocument document;
        document.setDefaultStyleSheet(theme::DocumentCss());
        document.setHtml(QStringLiteral("<p class='speaker'>Revia</p><p class='activityRow'>Remembered a preference</p>"
                                        "<p class='activityRow warn'>Needs your OK</p><p class='activityRow error'>Couldn't finish</p>"));
        struct ExpectedBlock
        {
            const char* text;
            QColor colour;
            int pixels;
            int weight;
        };
        const ExpectedBlock expected[] = {{"Revia", theme::Light().textSecondary, 13, QFont::DemiBold},
            {"Remembered a preference", theme::Light().textTertiary, 12, QFont::Normal},
            {"Needs your OK", theme::Light().warning, 12, QFont::Normal}, {"Couldn't finish", theme::Light().danger, 13, QFont::Normal}};
        QTextBlock block = document.begin();
        for (const ExpectedBlock& want : expected)
        {
            const QTextCharFormat format = block.isValid() ? block.begin().fragment().charFormat() : QTextCharFormat();
            const int pixels = format.property(QTextFormat::FontPixelSize).toInt();
            results.Expect(block.text() == QLatin1String(want.text) && format.foreground().color() == want.colour &&
                    pixels == want.pixels && format.fontWeight() == want.weight,
                std::string("transcript block '") + want.text + "' renders " + Describe(format.foreground().color()) + " at " +
                    std::to_string(pixels) + "px weight " + std::to_string(format.fontWeight()) + ", wants " + Describe(want.colour) +
                    " at " + std::to_string(want.pixels) + "px weight " + std::to_string(want.weight));
            block = block.next();
        }
    }

    // Light to Dark: palette, stylesheet, painted widgets, icons and listeners all follow.
    int listenerRuns = 0;
    const int listener = theme::AddListener([&listenerRuns]() { ++listenerRuns; });
    const QColor lightButton = PixelAt(sample, QPoint(sample->width() / 2, sample->height() / 2));
    theme::SetPreference(theme::Preference::Dark);
    Settle();
    const QColor darkButton = PixelAt(sample, QPoint(sample->width() / 2, sample->height() / 2));
    results.Expect(theme::CurrentScheme() == theme::Scheme::Dark && theme::CurrentPreference() == theme::Preference::Dark,
        "SetPreference(Dark) did not switch the scheme");
    results.Expect(QSettings().value("appearance/theme").toString() == QLatin1String("dark"), "SetPreference did not store the choice");
    results.Expect(QApplication::palette().color(QPalette::Window) == theme::Dark().background, "the dark palette was not applied");
    results.Expect(lightButton != darkButton && Near(lightButton, theme::Light().surface) && Near(darkButton, theme::Dark().surface),
        "a button's polished background did not change from light surface to dark surface: " + Describe(lightButton) + " -> " +
            Describe(darkButton));
    results.Expect(root->styleSheet() == theme::StyleSheet() && root->styleSheet().contains(theme::Dark().background.name()),
        "the root stylesheet was not rebuilt from the dark tokens");
    results.Expect(listenerRuns == 1, "a listener did not run exactly once for the scheme change");
    results.Expect(Near(PixelAt(onSwitch, trackLeft), theme::Dark().accent), "a dark 'on' switch is not the dark accent");
    results.Expect(Near(PixelAt(offSwitch, offKnob), theme::Dark().switchOff), "a dark 'off' switch knob is not the dark switchOff");
    results.Expect(Near(StrokeColour(iconButton->icon().pixmap(18, 18)), theme::Dark().textSecondary),
        "a bound icon did not follow the dark scheme");
    results.Expect(Near(StrokeColour(svgLabel->pixmap().copy()), theme::Dark().danger), "a bound SVG label did not follow the dark scheme");
    if (voiceState != nullptr && memoryState != nullptr && lookupState != nullptr)
        results.Expect(voiceState->foreground().color() == theme::Dark().danger &&
                memoryState->foreground().color() == theme::Dark().textSecondary &&
                lookupState->foreground().color() == theme::Dark().success,
            "state cells were not recoloured for the dark scheme");
    const QString darkMarkdown = revia::desktop::RenderUntrustedMarkdown("See [docs](https://example.com).");
    results.Expect(darkMarkdown.contains(theme::Dark().link.name()), "Markdown links do not use the dark link colour");
    results.Expect(theme::DocumentCss().contains(theme::Dark().text.name()), "DocumentCss does not carry the dark text colour");

    // Choosing what is already shown costs no restyle; a removed listener stays quiet.
    theme::SetPreference(theme::Preference::Dark);
    Settle();
    results.Expect(listenerRuns == 1, "re-selecting the current scheme restyled the window");
    theme::Install(root.get());
    QEvent themeChange(QEvent::ThemeChange);
    QApplication::sendEvent(root.get(), &themeChange);
    Settle();
    results.Expect(listenerRuns == 1, "a repeated Install or a system theme event without a change restyled the window");
    if (!renderDirectory.isEmpty())
        SaveGallery(root.get(), renderDirectory, "theme-gallery-dark.png");
    theme::SetPreference(theme::Preference::Light);
    Settle();
    results.Expect(listenerRuns == 2 && theme::CurrentScheme() == theme::Scheme::Light, "switching back to Light did not apply");
    if (!renderDirectory.isEmpty())
        SaveGallery(root.get(), renderDirectory, "theme-gallery-light.png");
    theme::RemoveListener(listener);
    theme::SetPreference(theme::Preference::Dark);
    theme::SetPreference(theme::Preference::Light);
    Settle();
    results.Expect(listenerRuns == 2, "a removed listener still ran");

    // A panel removes its listener when destroyed, so a later change cannot reach it.
    delete pipelines;
    theme::SetPreference(theme::Preference::Dark);
    theme::SetPreference(theme::Preference::Light);
    Settle();

    // High contrast: the system palette and the structure sheet only.
    theme::SetHighContrastForTest(true);
    Settle();
    results.Expect(theme::HighContrastActive(), "the high-contrast test hook did not take effect");
    results.Expect(root->styleSheet() == structure + QLatin1Char('\n') + contrastSheet,
        "high contrast did not swap the colour stylesheet for the structure and contrast sheets");
    results.Expect(QApplication::palette().color(QPalette::Window) != theme::Light().background,
        "high contrast kept the custom palette");
    results.Expect(theme::Current().background == QApplication::palette().color(QPalette::Window) &&
            theme::Current().accent == QApplication::palette().color(QPalette::Highlight),
        "high-contrast tokens are not taken from the system palette");
    {
        // The contrast sheet still gives overlays an opaque fill, checked buttons a fill and a
        // focused tool button a visible ring, all from the system palette.
        // A stand-in transcript in a colour no system palette uses, so an overlay that lets it
        // show through cannot pass by matching it.
        auto* page = new QTextBrowser(root.get());
        page->setStyleSheet(QStringLiteral("QTextBrowser { background: #ff00ff; border: none; padding: 0px; }"));
        page->setGeometry(0, 0, 360, 160);
        auto* toast = new QFrame(root.get());
        toast->setObjectName("toast");
        auto* toastLayout = new QHBoxLayout(toast);
        auto* toastLabel = new QLabel("Turned off. Change it in Settings.", toast);
        toastLabel->setObjectName("toastLabel");
        toastLayout->addWidget(toastLabel);
        toast->setGeometry(20, 20, 300, 48);
        auto* overlay = new QLabel("Switching to Mira", root.get());
        overlay->setObjectName("navigationLockOverlay");
        overlay->setAlignment(Qt::AlignCenter);
        overlay->setGeometry(0, 80, 360, 80);
        auto* onButton = new QPushButton("On", root.get());
        onButton->setCheckable(true);
        onButton->setChecked(true);
        onButton->setGeometry(380, 20, 80, 32);
        auto* offButton = new QPushButton("Off", root.get());
        offButton->setCheckable(true);
        offButton->setGeometry(470, 20, 80, 32);
        auto* contrastTool = new QToolButton(root.get());
        theme::BindIcon(contrastTool, "settings");
        contrastTool->setGeometry(560, 20, 32, 32);
        auto* contrastSpin = new InspectableSpinBox(root.get());
        contrastSpin->setValue(12);
        contrastSpin->setGeometry(QRect(QPoint(380, 70), QSize(120, contrastSpin->sizeHint().height())));
        for (QWidget* widget : std::initializer_list<QWidget*>{page, toast, overlay, onButton, offButton, contrastTool, contrastSpin})
        {
            widget->show();
            widget->raise();
        }
        Settle();
        const QPoint inToast = toast->geometry().topLeft() + QPoint(toast->width() / 2, 3);
        const QPoint inOverlay = overlay->geometry().topLeft() + QPoint(16, 16);
        const QColor toastShown = PixelAt(root.get(), inToast);
        const QColor overlayShown = PixelAt(root.get(), inOverlay);
        toast->hide();
        overlay->hide();
        Settle();
        const QColor toastHidden = PixelAt(root.get(), inToast);
        const QColor overlayHidden = PixelAt(root.get(), inOverlay);
        results.Expect(toastShown != toastHidden && overlayShown != overlayHidden,
            "under high contrast the toast or the navigation lock overlay lets the transcript show through: toast " +
                Describe(toastShown) + " over " + Describe(toastHidden) + ", overlay " + Describe(overlayShown) + " over " +
                Describe(overlayHidden));
        const QPoint inButton(4, onButton->height() / 2);
        results.Expect(PixelAt(onButton, inButton) != PixelAt(offButton, inButton),
            "under high contrast a checked button looks the same as an unchecked one: " + Describe(PixelAt(onButton, inButton)));
        // A gallery window may have taken activation since the focus checks above.
        root->activateWindow();
        sample->setFocus(Qt::OtherFocusReason);
        Settle();
        const QImage toolRest = contrastTool->grab().toImage();
        contrastTool->setFocus(Qt::TabFocusReason);
        Settle();
        results.Expect(contrastTool->grab().toImage() != toolRest, "under high contrast a focused tool button shows no ring");
        sample->setFocus(Qt::OtherFocusReason);
        {
            // The style draws a spin box's arrows itself: in the middle of the up button, away from
            // its edges, some pixels must stand out from the button at 3:1, as a glyph must.
            const QImage spin = contrastSpin->grab().toImage().convertToFormat(QImage::Format_ARGB32);
            const qreal ratio = spin.devicePixelRatio();
            const QRect up = contrastSpin->UpButton();
            const QRect middle = up.adjusted(up.width() / 4, up.height() / 4, -up.width() / 4, -up.height() / 4);
            const QRect band(qRound(middle.x() * ratio), qRound(middle.y() * ratio), qRound(middle.width() * ratio),
                qRound(middle.height() * ratio));
            std::map<QRgb, int> counts;
            for (int y = band.top(); y <= band.bottom(); ++y)
                for (int x = band.left(); x <= band.right(); ++x)
                    ++counts[spin.pixel(x, y)];
            const auto common = std::max_element(counts.begin(), counts.end(),
                [](const auto& first, const auto& second) { return first.second < second.second; });
            int glyph = 0;
            for (const auto& [pixel, count] : counts)
                if (common != counts.end() && theme::ContrastRatio(QColor(pixel), QColor(common->first)) >= 3.0)
                    glyph += count;
            results.Expect(!up.isEmpty() && glyph >= 3,
                "under high contrast a spin box shows no arrows (" + std::to_string(glyph) + " glyph pixels)");
        }
        for (QWidget* widget : std::initializer_list<QWidget*>{page, toast, overlay, onButton, offButton, contrastTool, contrastSpin})
            delete widget;
        Settle();
    }
    if (!renderDirectory.isEmpty())
        SaveGallery(root.get(), renderDirectory, "theme-gallery-high-contrast.png");
    {
        // Windows can move from one contrast theme to another (Night sky to Desert) while the
        // high-contrast flag stays set; only the system palette changes. An explicit application
        // palette stands in for the new system one here.
        int contrastRuns = 0;
        const int contrastListener = theme::AddListener([&contrastRuns]() { ++contrastRuns; });
        QPalette nightSky = QApplication::palette();
        for (const auto& [role, colour] : {std::pair{QPalette::Window, QColor("#000000")}, std::pair{QPalette::WindowText, QColor("#FFFFFF")},
                 std::pair{QPalette::Base, QColor("#000000")}, std::pair{QPalette::Text, QColor("#FFFFFF")},
                 std::pair{QPalette::Button, QColor("#000000")}, std::pair{QPalette::ButtonText, QColor("#FFFF00")},
                 std::pair{QPalette::Highlight, QColor("#1AEBFF")}, std::pair{QPalette::HighlightedText, QColor("#000000")}})
            nightSky.setColor(role, colour);
        QApplication::setPalette(nightSky);
        QEvent contrastChange(QEvent::ThemeChange);
        QApplication::sendEvent(root.get(), &contrastChange);
        Settle();
        results.Expect(theme::HighContrastActive() && theme::Current().background == QColor("#000000") &&
                theme::Current().accent == QColor("#1AEBFF") && theme::Current().textSecondary == QColor("#FFFF00"),
            "a change of contrast theme did not reach the theme's tokens: background " + Describe(theme::Current().background));
        results.Expect(Near(StrokeColour(iconButton->icon().pixmap(18, 18)), QColor("#FFFF00")) &&
                Near(StrokeColour(svgLabel->pixmap().copy()), QColor("#FFFFFF")),
            "a change of contrast theme did not recolour bound icons and labels");
        results.Expect(contrastRuns == 1, "a change of contrast theme did not run the listeners once");
        QApplication::setPalette(QPalette());
        QApplication::sendEvent(root.get(), &contrastChange);
        Settle();
        results.Expect(theme::Current().background == QApplication::palette().color(QPalette::Window) && contrastRuns == 2,
            "returning to the system's contrast palette did not refresh the tokens");
        theme::RemoveListener(contrastListener);
    }
    theme::SetHighContrastForTest(std::nullopt);
    Settle();
    results.Expect(!theme::HighContrastActive() && QApplication::palette().color(QPalette::Window) == theme::Light().background &&
            root->styleSheet() == theme::StyleSheet(theme::Light()),
        "leaving high contrast did not restore the light theme");

    // Menus and native frames are no-ops off Windows but must accept any window.
    auto* menu = new QMenu(root.get());
    theme::PolishMenu(menu);
    theme::PolishMenu(menu);
    results.Expect(menu->property("reviaMenuPolished").toBool(), "PolishMenu did not mark the menu");
    theme::ApplyNativeFrame(root.get());
    theme::ApplyNativeFrame(nullptr);

    // A window that starts with no other window alive reads the preference again, and the
    // environment still wins over the stored choice.
    root.reset();
    Settle();
    qputenv("REVIA_THEME", "dark");
    QSettings().setValue("appearance/theme", "light");
    auto second = std::make_unique<QWidget>();
    theme::Install(second.get());
    results.Expect(theme::CurrentPreference() == theme::Preference::Dark && theme::CurrentScheme() == theme::Scheme::Dark &&
            second->styleSheet().contains(theme::Dark().background.name()),
        "REVIA_THEME=dark did not win for a new window");
    second.reset();
    qunsetenv("REVIA_THEME");
    auto third = std::make_unique<QWidget>();
    theme::Install(third.get());
    results.Expect(theme::CurrentPreference() == theme::Preference::Light && theme::CurrentScheme() == theme::Scheme::Light,
        "without REVIA_THEME the stored preference did not apply");
    third.reset();
    if (originalTheme.isNull())
        qunsetenv("REVIA_THEME");
    else
        qputenv("REVIA_THEME", originalTheme);

    std::cout << (results.failures == 0 ? "Revia.Theme passed\n" : "Revia.Theme failed\n");
    return results.failures;
}
} // namespace

REVIA_DESKTOP_SUITE("theme", RunThemeSuite);
