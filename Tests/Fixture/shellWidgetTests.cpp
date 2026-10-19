// Revia.ShellWidgets: the shell's own widgets on their own, without a ReviaWindow: panel windows
// and their host, the command palette, the sidebar, the composer card, the toast, the painted
// shadow and the shortcut sheet. Runs as `ReviaDesktopStopTests --suite=shell`, inside main()'s
// QApplication with temporary QSettings. REVIA_UI_RENDER_DIR=<dir> also saves captures of the
// sidebar, the composer, the palette and a panel window hosting a stand-in page, for review.
#include "desktopSuites.h"
#include "desktopObjectNames.h"
#include "commandPalette.h"
#include "composerCard.h"
#include "panelWindow.h"
#include "settingsTypes.h"
#include "shellSidebar.h"
#include "shortcutSheet.h"
#include "softShadow.h"
#include "toastOverlay.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace revia::desktop;

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

void Wait(const int milliseconds)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    Settle();
}

void SendKey(QWidget* target, const int key, const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QCoreApplication::sendEvent(target, &release);
}

void Press(QWidget* target, const QPoint position)
{
    const QPointF local(position);
    const QPointF global(target->mapToGlobal(position));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
}

void Click(QWidget* target, const QPoint position)
{
    Press(target, position);
    const QPointF local(position);
    const QPointF global(target->mapToGlobal(position));
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &release);
}

// Every objectName a shell widget gives itself must be one desktopObjectNames.h lists, so the
// stylesheet has a rule for it. Qt's internal children and the test's own stand-ins are skipped.
void ExpectListedNames(Results& results, QWidget* root, const std::string& what)
{
    std::set<QString> known;
    for (const auto& name : names::kStyled)
        known.insert(QString::fromUtf8(name.name.data(), static_cast<qsizetype>(name.name.size())));
    for (const auto& name : names::kUnstyled)
        known.insert(QString::fromUtf8(name.name.data(), static_cast<qsizetype>(name.name.size())));
    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    widgets.prepend(root);
    for (const QWidget* widget : widgets)
    {
        const QString name = widget->objectName();
        if (name.isEmpty() || name.startsWith("qt_") || name.startsWith("test") ||
            name.startsWith(QString::fromUtf8(names::kPanelWindowPrefix.data(), static_cast<qsizetype>(names::kPanelWindowPrefix.size()))))
            continue;
        bool insideStandIn = false;
        for (const QWidget* ancestor = widget->parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget())
            insideStandIn = insideStandIn || ancestor->objectName().startsWith("test");
        if (!insideStandIn)
            results.Expect(known.count(name) == 1, what + " uses an objectName desktopObjectNames.h does not list: " + name.toStdString());
    }
}

void ExpectPresent(Results& results, QWidget* root, const std::vector<const char*>& wanted, const std::string& what)
{
    for (const char* name : wanted)
        results.Expect(root->objectName() == name || root->findChild<QWidget*>(name) != nullptr, what + " has no " + name);
}

void Capture(QWidget* widget, const QString& file)
{
    const QString directory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
    if (directory.isEmpty() || widget == nullptr)
        return;
    QDir().mkpath(directory);
    widget->grab().save(QDir(directory).filePath(file));
}

QIcon Swatch(const QColor& colour)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colour, 1.75));
    painter.drawRoundedRect(QRectF(3.5, 3.5, 11, 11), 3, 3);
    return QIcon(pixmap);
}

void TestFuzzyScore(Results& results)
{
    results.Expect(FuzzyScore("mem", "Memory") == 1000, "a prefix scores 1000");
    results.Expect(FuzzyScore("MEM", "memory") == 1000, "matching ignores case");
    results.Expect(FuzzyScore("mem", "Open Memory") == 600, "a word start scores 600");
    results.Expect(FuzzyScore("mem", "Settings › Memory") == 600, "a word after punctuation is a word start");
    results.Expect(FuzzyScore("mem", "Remember") == 300, "a substring scores 300");
    const int subsequence = FuzzyScore("mem", "Theme: System");
    results.Expect(subsequence > 0 && subsequence < 100, "a scattered match scores under 100");
    results.Expect(FuzzyScore("ac", "abc") > FuzzyScore("ac", "abbbc"), "fewer gaps score higher");
    results.Expect(FuzzyScore("xyz", "Memory") == -1, "no match scores -1");
    results.Expect(FuzzyScore("memoryx", "Memory") == -1, "a query longer than the text does not match");
    results.Expect(FuzzyScore("", "Memory") == 0 && FuzzyScore("   ", "Memory") == 0, "an empty query scores 0");
    results.Expect(FuzzyScore("mem", "Memory") > FuzzyScore("mem", "Open Memory") &&
                       FuzzyScore("mem", "Open Memory") > FuzzyScore("mem", "Remember") &&
                       FuzzyScore("mem", "Remember") > FuzzyScore("mem", "Theme: System"),
        "prefix > word start > substring > subsequence");
}

void TestPalette(Results& results)
{
    QSettings().remove("palette/recent");
    QWidget host;
    host.setObjectName("testPaletteHost");
    host.resize(900, 600);
    host.show();
    auto* before = new QLineEdit(&host);
    before->setObjectName("testFocusBefore");
    before->show();
    before->setFocus();
    auto* palette = new CommandPalette(&host);
    // After the palette in the tab chain, so hiding the palette alone would focus this one.
    auto* after = new QLineEdit(&host);
    after->setObjectName("testFocusAfter");
    after->show();
    QStringList ran;
    bool locked = false;
    palette->SetAvailable([&locked]() { return !locked; });
    const auto entry = [&ran](const QString& id, const QString& title, const QString& group, const QString& subtitle = {})
    {
        CommandEntry command;
        command.id = id;
        command.title = title;
        command.group = group;
        command.subtitle = subtitle;
        command.run = [&ran, id]() { ran.push_back(id); };
        return command;
    };
    std::vector<CommandEntry> commands;
    commands.push_back(entry("action:newChat", "New chat", "Actions"));
    commands.back().shortcutText = "Ctrl+N";
    commands.back().icon = Swatch(QColor("#0D6B64"));
    commands.push_back(entry("theme:system", "Theme: System", "Actions"));
    commands.push_back(entry("action:stopDesktop", "Stop desktop control", "Actions"));
    commands.back().enabled = []() { return false; };
    commands.push_back(entry("panel:memory", "Open Memory", "Panels", "Window"));
    commands.back().keywords = {"memory"};
    commands.back().shortcutText = "Ctrl+Shift+M";
    commands.push_back(entry("panel:vision", "Open Vision", "Panels", "Window"));
    commands.push_back(entry("setting:profile.memoryEnabled", "Remember things (this profile)", "Settings", "Settings › Memory"));
    commands.push_back(entry("companion:ada", "Switch to Ada", "Companions"));
    palette->SetCommands(commands);
    int dynamicCalls = 0;
    QString lastDynamicQuery;
    palette->SetDynamicSource(
        [&](const QString& query)
        {
            ++dynamicCalls;
            lastDynamicQuery = query;
            std::vector<CommandEntry> chats;
            for (const QString& title : {QStringLiteral("Trip planning"), QStringLiteral("Tax questions"), QStringLiteral("Bread recipe")})
            {
                if (query.isEmpty() || title.contains(query, Qt::CaseInsensitive))
                    chats.push_back(entry("chat:" + title, title, {}, "Chat"));
            }
            return chats;
        });
    auto* input = palette->findChild<QLineEdit*>("commandPaletteInput");
    auto* list = palette->findChild<QListWidget*>("commandPaletteList");
    auto* card = palette->findChild<QWidget*>("commandPalette");
    auto* empty = palette->findChild<QLabel*>("commandPaletteEmpty");
    results.Expect(palette->objectName() == "commandPaletteScrim", "the palette overlay is commandPaletteScrim");
    results.Expect(input && list && card && empty && palette->findChild<QLabel*>("commandPaletteFooter"), "the palette has its parts");
    if (!input || !list || !card || !empty)
        return;

    Settle(); // the host active, with focus in testFocusBefore
    palette->Open();
    Settle();
    results.Expect(palette->IsOpen() && palette->geometry() == host.rect(), "the open palette covers its host");
    results.Expect(card->width() == 640 && card->y() == 90, "the card is min(640, width - 48) wide, 15% from the top");
    results.Expect(list->item(0) != nullptr && list->item(0)->flags() == Qt::NoItemFlags && list->item(0)->text() == "Actions",
        "the first row is the Actions header");
    results.Expect(palette->CurrentId() == "action:newChat", "the first entry, not its header, is highlighted");
    results.Expect(lastDynamicQuery.isEmpty() && dynamicCalls == 1, "chats are asked for at once on opening");
    std::vector<QString> visited{palette->CurrentId()};
    for (int step = 0; step < 6; ++step)
    {
        SendKey(input, Qt::Key_Down);
        visited.push_back(palette->CurrentId());
    }
    results.Expect(
        std::none_of(visited.begin(), visited.end(), [](const QString& id) { return id.isEmpty(); }), "Down never stops on a header");
    results.Expect(std::find(visited.begin(), visited.end(), "action:stopDesktop") == visited.end(), "Down passes over disabled entries");
    results.Expect(visited.size() > 2 && visited[1] == "theme:system" && visited[2] == "panel:memory",
        "Down crosses from the last Action past the Panels header");
    SendKey(input, Qt::Key_Up);
    results.Expect(palette->CurrentId() == visited[5], "Up moves back one entry");
    for (int step = 0; step < 12; ++step)
        SendKey(input, Qt::Key_Up);
    results.Expect(palette->CurrentId() == "action:newChat", "Up stops at the first entry");
    SendKey(input, Qt::Key_Tab);
    results.Expect(palette->IsOpen() && palette->CurrentId() == "theme:system", "Tab moves within the palette instead of leaving it");
    SendKey(input, Qt::Key_Escape);
    results.Expect(!palette->IsOpen() && ran.isEmpty(), "Esc closes without running anything");
    Settle();
    results.Expect(before->hasFocus(), "closing the palette gives focus back to where it was");

    palette->Open("mem");
    Settle();
    results.Expect(palette->CurrentId() == "panel:memory", "\"mem\" highlights Open Memory, the best match");
    results.Expect(list->item(0) != nullptr && list->item(0)->text() == "Panels", "with a query the best group comes first");
    SendKey(input, Qt::Key_Return);
    results.Expect(!palette->IsOpen() && ran == QStringList{"panel:memory"}, "Enter runs the highlighted entry and closes");
    results.Expect(QSettings().value("palette/recent").toStringList().value(0) == "panel:memory", "a run entry is remembered");

    palette->Open();
    Settle();
    results.Expect(list->item(0) != nullptr && list->item(0)->text() == "Recent" && palette->CurrentId() == "panel:memory",
        "with no query Recent comes first");
    int memoryRows = 0;
    for (int row = 0; row < list->count(); ++row)
        memoryRows += list->item(row)->text() == "Open Memory" ? 1 : 0;
    results.Expect(memoryRows == 1, "a recent entry is not listed twice");
    input->setText("zzzz");
    Settle();
    results.Expect(empty->isVisibleTo(palette) && !list->isVisibleTo(palette), "no match shows No matches");
    SendKey(input, Qt::Key_Return);
    results.Expect(ran.size() == 1, "Enter with no match runs nothing");
    input->setText("stop desktop");
    Settle();
    results.Expect(list->count() == 2 && palette->CurrentId().isEmpty(), "a disabled match is listed but not highlighted");
    SendKey(input, Qt::Key_Return);
    results.Expect(ran.size() == 1, "a disabled entry does not run");

    // Typing asks the chat source once the query settles, not on every keystroke.
    const int callsBefore = dynamicCalls;
    input->setText("t");
    input->setText("ta");
    input->setText("tax");
    results.Expect(dynamicCalls == callsBefore, "chats are not asked for while typing");
    Wait(260);
    results.Expect(dynamicCalls == callsBefore + 1 && lastDynamicQuery == "tax", "chats are asked for 150 ms after typing stops");
    bool foundChat = false;
    for (int row = 0; row < list->count(); ++row)
        foundChat = foundChat || list->item(row)->text() == "Tax questions";
    results.Expect(foundChat, "the chat source's entries are listed");
    palette->Close();

    palette->OpenChats();
    Settle();
    results.Expect(palette->IsChatsMode() && input->placeholderText() == "Search chats", "chats mode says what it searches");
    bool onlyChats = list->count() > 0;
    for (int row = 0; row < list->count(); ++row)
    {
        const QString text = list->item(row)->text();
        onlyChats = onlyChats && (text == "Chats" || text == "Trip planning" || text == "Tax questions" || text == "Bread recipe");
    }
    results.Expect(onlyChats, "chats mode lists only chats");

    // A click on an entry runs it; a click outside the card closes.
    const int breadRow = [&list]()
    {
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->text() == "Bread recipe")
                return row;
        return -1;
    }();
    if (breadRow >= 0)
    {
        Click(list->viewport(), list->visualItemRect(list->item(breadRow)).center());
        Settle();
        results.Expect(ran.value(ran.size() - 1) == "chat:Bread recipe" && !palette->IsOpen(), "clicking an entry runs it");
    }
    palette->Open();
    Settle();
    // The footer takes no clicks, so this one reaches the scrim from inside the card.
    Press(palette->findChild<QLabel*>("commandPaletteFooter"), QPoint(4, 4));
    Settle();
    results.Expect(palette->IsOpen(), "a click inside the card leaves the palette open");
    Press(palette, QPoint(4, 4));
    Settle();
    results.Expect(!palette->IsOpen(), "a click on the scrim closes the palette");

    // The chat source may answer with more than fits; the palette lists eight.
    palette->SetDynamicSource(
        [&entry](const QString&)
        {
            std::vector<CommandEntry> many;
            for (int index = 1; index <= 12; ++index)
                many.push_back(entry(QStringLiteral("chat:%1").arg(index), QStringLiteral("Chat %1").arg(index), {}, "Chat"));
            return many;
        });
    palette->OpenChats();
    Settle();
    int chatRows = 0;
    for (int row = 0; row < list->count(); ++row)
        chatRows += list->item(row)->text().startsWith("Chat ") ? 1 : 0;
    results.Expect(chatRows == 8, "at most eight chats are listed");
    palette->Close();
    host.resize(500, 400);
    palette->Open();
    Settle();
    results.Expect(card->width() == 452 && card->y() == 60, "the card narrows with its host");
    Capture(&host, "shell-palette-narrow.png");
    palette->Close();

    // Locking while the palette is open (a companion switch, shutting down) stops it running
    // anything, from the keyboard or a click.
    palette->Open("theme");
    Settle();
    const qsizetype ranBeforeLock = ran.size();
    locked = true;
    SendKey(input, Qt::Key_Return);
    results.Expect(!palette->IsOpen() && ran.size() == ranBeforeLock, "an open palette runs nothing once navigation locks; it closes");
    palette->Open();
    results.Expect(!palette->IsOpen(), "the palette does not open while navigation is locked");
    locked = false;
    ExpectListedNames(results, palette, "the command palette");
}

// The window's frame is the planned rectangle, or, when the planned one is smaller than the
// window's minimum size allows, that minimum frame aligned to the plan's right and bottom edges.
bool FitsPlan(const QWidget* window, const QRect& plan)
{
    const QRect frame = window->frameGeometry();
    const QSize extra = frame.size() - window->size();
    const QSize expected = plan.size().expandedTo(window->minimumSize() + extra);
    return frame.size() == expected && frame.right() == plan.right() && frame.bottom() == plan.bottom();
}

void TestPlacement(Results& results)
{
    const QRect screen(0, 0, 1920, 1040);
    const QSize size(640, 560);
    results.Expect(PanelPlacement({100, 80, 1000, 700}, screen, size, 0) == QRect(1112, 80, 640, 560),
        "a panel opens 12px right of the owner, top-aligned");
    results.Expect(
        PanelPlacement({100, 80, 1000, 700}, screen, size, 1) == QRect(1112, 112, 640, 560), "the next panel on that side steps down 32px");
    results.Expect(PanelPlacement({700, 80, 1000, 700}, screen, size, 0) == QRect(48, 80, 640, 560),
        "without room on the right a panel opens on the left");
    results.Expect(PanelPlacement({300, 80, 1400, 700}, screen, size, 0) == QRect(1280, 80, 640, 560),
        "without room on either side a panel is flush with the right edge");
    results.Expect(PanelPlacement({100, 500, 1000, 500}, screen, size, 5) == QRect(1112, 480, 640, 560),
        "the cascade stays inside the available area");
    results.Expect(PanelPlacement({0, 0, 800, 600}, {0, 0, 800, 600}, {900, 700}, 0).size() == QSize(800, 600),
        "a panel larger than the screen is clamped to it");
    results.Expect(PanelPlacement({2000, 50, 700, 600}, {1920, 0, 1280, 1000}, size, 0) == QRect(2560, 50, 640, 560),
        "placement respects a second monitor's origin");

    const BesidePlacement wide = PlaceBesideGeometry({300, 100, 1180, 780}, screen, 640, 480);
    results.Expect(wide.ownerFrame == QRect(0, 100, 1268, 780) && wide.panelFrame == QRect(1280, 100, 640, 780),
        "Place beside chat splits a wide screen into owner, 12px gap and panel");
    const BesidePlacement laptop = PlaceBesideGeometry({100, 0, 1200, 700}, {0, 0, 1366, 728}, 720, 560);
    results.Expect(laptop.panelFrame.width() == 560 && laptop.ownerFrame.width() == 794 && laptop.panelFrame.x() == 806,
        "the panel gets 40% of the screen but never less than its useful width");
    const BesidePlacement small = PlaceBesideGeometry({100, 0, 900, 650}, {0, 0, 1024, 700}, 600, 480);
    results.Expect(small.ownerFrame.width() == 760 && small.panelFrame == QRect(544, 0, 480, 650),
        "the owner never narrows below 760; the panel then overlaps it at the right edge");
}

void TestPanelCatalog(Results& results)
{
    const auto& catalog = PanelCatalog();
    results.Expect(catalog.size() == 15, "the catalog lists the 15 pop-up panels");
    std::set<std::string> keys;
    for (std::size_t index = 0; index < catalog.size(); ++index)
    {
        const PanelSpec& spec = catalog[index];
        results.Expect(static_cast<std::size_t>(spec.id) == index && &Spec(spec.id) == &spec, std::string("catalog order for ") + spec.key);
        keys.insert(spec.key);
        results.Expect(PanelFromKey(spec.key) == spec.id && PanelFromKey(QString::fromUtf8(spec.title).toUpper()) == spec.id,
            std::string("PanelFromKey finds ") + spec.key);
        results.Expect(spec.defaultSize.width() >= 480 && spec.defaultSize.height() >= 400 && spec.minUsefulWidth >= 480,
            std::string("useful sizes for ") + spec.key);
        results.Expect(spec.iconName != nullptr && spec.pageObjectName != nullptr && spec.group != nullptr,
            std::string("icon, page and group for ") + spec.key);
    }
    results.Expect(keys.size() == catalog.size(), "panel keys are unique");
    results.Expect(!PanelFromKey("nonsense").has_value(), "an unknown key finds nothing");
    results.Expect(Spec(PanelId::Memory).shortcut == QKeySequence("Ctrl+Shift+M") &&
                       Spec(PanelId::Activity).shortcut == QKeySequence("Ctrl+Shift+A") && Spec(PanelId::Voice).shortcut.isEmpty(),
        "only Memory and Activity carry shortcuts");
    results.Expect(
        Spec(PanelId::InternetActivity).key == std::string("internet") && Spec(PanelId::Memory).pageObjectName == std::string("memoryTab"),
        "keys and pages follow the spec table");
    // The settings sections are plain ints in the catalog; they must stay in step with the enum.
    const auto section = [](const SettingsSection value) { return static_cast<int>(value); };
    results.Expect(Spec(PanelId::Vision).settingsSection == section(SettingsSection::Vision) &&
                       Spec(PanelId::Profiles).settingsSection == section(SettingsSection::Chat) &&
                       Spec(PanelId::Memory).settingsSection == section(SettingsSection::Memory) &&
                       Spec(PanelId::Voice).settingsSection == section(SettingsSection::Voice) &&
                       Spec(PanelId::Presence).settingsSection == section(SettingsSection::Integrations) &&
                       Spec(PanelId::InternetActivity).settingsSection == section(SettingsSection::Internet) &&
                       Spec(PanelId::Resources).settingsSection == section(SettingsSection::Advanced) &&
                       Spec(PanelId::Canvas).settingsSection == -1,
        "panel settings sections match SettingsSection");
}

void TestPanelWindows(Results& results)
{
    QSettings().remove("panels");
    QWidget owner;
    owner.setObjectName("testOwner");
    owner.resize(760, 540);
    owner.show();
    // In tab order: first, the message box (the default focus), then a field the test hides.
    auto* first = new QLineEdit(&owner);
    first->setObjectName("testFirstField");
    auto* message = new QPlainTextEdit(&owner);
    message->setObjectName("testMessage");
    auto* other = new QLineEdit(&owner);
    other->setObjectName("testOtherField");
    for (QWidget* widget : {static_cast<QWidget*>(first), static_cast<QWidget*>(message), static_cast<QWidget*>(other)})
        widget->show();
    auto host = std::make_unique<PanelHost>(&owner);
    host->SetDefaultFocus(message);
    Settle();
    message->setFocus();
    Settle();
    const QRect available = owner.screen()->availableGeometry();

    // A page that lives in a tab widget and is not its current tab, so it is explicitly hidden,
    // like the pages the window takes out of its old tab tree.
    auto* tabs = new QTabWidget(&owner);
    tabs->setObjectName("testTabs");
    auto* page = new QWidget;
    page->setObjectName("testCanvasPage");
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 4, 0, 0);
    auto* title = new QLabel("Canvas", page);
    title->setObjectName("testRepeatedTitle");
    pageLayout->addWidget(title);
    auto* field = new QLineEdit(page);
    field->setObjectName("testField");
    pageLayout->addWidget(field);
    auto* notes = new QTextEdit(page);
    notes->setObjectName("testNotes");
    pageLayout->addWidget(notes);
    tabs->addTab(page, "Canvas");
    tabs->addTab(new QWidget, "Other");
    tabs->setCurrentIndex(1);
    results.Expect(page->isHidden(), "the stand-in page starts hidden by its tab widget");

    host->Adopt(PanelId::Canvas, page);
    PanelWindow* window = host->Window(PanelId::Canvas);
    QPointer<PanelWindow> guard(window);
    results.Expect(window != nullptr, "Adopt creates the panel's window");
    if (window == nullptr)
        return;
    results.Expect(window->objectName() == "panelWindow_canvas" && window->property(names::kRoleProperty).toString() == "panelWindow",
        "the window is panelWindow_<key> with reviaRole=panelWindow");
    results.Expect(window->isWindow() && window->parentWidget() == &owner, "the window is a top-level child of the owner");
    results.Expect(owner.findChild<QWidget*>("testField") == field, "the owner still finds the hosted page's widgets");
    results.Expect(window->windowTitle() == "Canvas — Revia", "the window title names the panel and the app");
    results.Expect(!window->testAttribute(Qt::WA_DeleteOnClose) && !window->testAttribute(Qt::WA_QuitOnClose),
        "closing a panel neither deletes it nor quits");
    results.Expect(window->minimumSize() == QSize(480, 400), "panel windows are at least 480x400");
    results.Expect(window->findChild<QWidget*>("panelHeader") && window->findChild<QToolButton*>("panelPlaceBeside") &&
                       !window->findChild<QToolButton*>("panelOpenSettings"),
        "the header has Place beside chat, and no gear for a panel without a Settings section");
    results.Expect(page->layout()->contentsMargins() == QMargins(20, 16, 20, 16), "the page margins are raised to 20/16");

    host->Adopt(PanelId::Canvas, page);
    host->Open(PanelId::Canvas);
    host->Open(PanelId::Canvas);
    Settle();
    const QRect canvasFrame = window->frameGeometry();
    results.Expect(available.contains(canvasFrame) && window->size() == Spec(PanelId::Canvas).defaultSize,
        "the first panel opens at its default size with its whole frame on the available area");
    int canvasWindows = 0;
    for (const QWidget* child : owner.findChildren<QWidget*>())
        canvasWindows += child->objectName() == "panelWindow_canvas" ? 1 : 0;
    results.Expect(canvasWindows == 1 && host->Window(PanelId::Canvas) == window, "a panel has a single window");
    results.Expect(
        window->isVisible() && page->isVisible() && window->Page() == page, "a page taken from a tab widget shows with its window");
    results.Expect(window->focusWidget() == window, "an opened panel holds focus itself, not on a header button");
    results.Expect(host->IsOpen(PanelId::Canvas) && host->Action(PanelId::Canvas)->isChecked(), "the panel's action is checked while open");
    results.Expect(host->IdOf(page) == PanelId::Canvas && host->IdOf(field) == PanelId::Canvas && !host->IdOf(&owner).has_value(),
        "IdOf maps a page and its descendants to the panel");
    results.Expect(host->Pages().size() == 1 && host->Pages().front() == page, "Pages lists the hosted pages");

    field->setText("half-typed");
    field->setFocus();
    SendKey(field, Qt::Key_Escape);
    results.Expect(window->isVisible(), "Esc in a field with text does not close the panel");
    field->clear();
    SendKey(field, Qt::Key_Escape);
    results.Expect(!window->isVisible() && guard, "Esc in an empty field closes (hides) the panel");
    results.Expect(!host->Action(PanelId::Canvas)->isChecked(), "the action unchecks when the window hides");
    Settle();
    results.Expect(owner.isActiveWindow() && message->hasFocus(), "closing a panel gives focus back to the owner's message box");
    // The owner's last focus widget went while the panel was open: focus goes to the default, not
    // to the first widget in the tab chain, which is all activating the owner would give.
    other->setFocus();
    Settle();
    host->Open(PanelId::Canvas);
    Settle();
    other->hide();
    host->Close(PanelId::Canvas);
    Settle();
    results.Expect(message->hasFocus(), "with the last focus widget gone, closing a panel focuses the default");
    other->show();
    host->Open(PanelId::Canvas);
    notes->setPlainText("notes");
    notes->setFocus();
    SendKey(notes, Qt::Key_Escape);
    results.Expect(window->isVisible(), "Esc in a text editor with text does not close the panel");
    SendKey(notes, Qt::Key_W, Qt::ControlModifier);
    results.Expect(!window->isVisible(), "Ctrl+W closes the panel");
    host->Open(PanelId::Canvas);
    window->close();
    Settle();
    results.Expect(!window->isVisible() && guard && host->Window(PanelId::Canvas) == window, "closing hides the window, never deletes it");

    // Geometry is saved on hide and restored by the first show of a window for the same panel.
    host->Open(PanelId::Canvas);
    window->setGeometry(30, 40, 520, 430);
    Settle();
    const QRect placed = window->geometry();
    host->Close(PanelId::Canvas);
    results.Expect(QSettings().value("panels/canvas/geometry").toRect() == placed, "the geometry is saved to panels/canvas/geometry");
    {
        QWidget otherOwner;
        auto* again = new PanelWindow(Spec(PanelId::Canvas), &otherOwner);
        again->show();
        Settle();
        results.Expect(again->geometry() == placed, "the saved geometry round-trips through the INI file");
        again->hide();
    }
    QSettings().setValue("panels/vision/geometry", QRect(-6000, -6000, 500, 420));
    auto* visionPage = new QWidget;
    visionPage->setObjectName("testVisionPage");
    auto* visionLayout = new QVBoxLayout(visionPage);
    // Vision's catalog entry names this heading as one that only repeats the window title.
    auto* visionTitle = new QLabel("Vision", visionPage);
    visionTitle->setObjectName("sectionTitle");
    visionLayout->addWidget(visionTitle);
    int shows = 0;
    std::vector<QSize> resizes;
    host->SetShowHook([&shows](QWidget*) { ++shows; });
    host->SetResizeHook(PanelId::Vision, [&resizes](const QSize size) { resizes.push_back(size); });
    host->Adopt(PanelId::Vision, visionPage);
    PanelWindow* vision = host->Window(PanelId::Vision);
    host->Open(PanelId::Vision);
    Settle();
    results.Expect(vision->geometry().intersects(QRect(0, 0, 800, 600)) && !QSettings().contains("panels/vision/geometry"),
        "a saved geometry on no screen is discarded and the panel is placed afresh");
    results.Expect(vision->frameGeometry().y() - canvasFrame.y() == 32 && vision->frameGeometry().right() == canvasFrame.right(),
        "the second panel placed on the same side steps down 32px");
    results.Expect(visionTitle->isHidden(), "a heading that repeats the window title is hidden");
    results.Expect(!resizes.empty() && resizes.back() == vision->size(), "the resize hook runs when the window shows");
    vision->resize(560, 450);
    Settle();
    results.Expect(!resizes.empty() && resizes.back() == QSize(560, 450), "the resize hook runs on resize");
    host->Close(PanelId::Vision);
    host->Open(PanelId::Vision);
    results.Expect(shows == 1, "the show hook runs once, on the first show");

    auto* gear = vision->findChild<QToolButton*>("panelOpenSettings");
    int section = -2;
    host->SetSettingsHook([&section](const int value) { section = value; });
    results.Expect(gear != nullptr && gear->toolTip() == "Open Vision settings", "a panel with a Settings section has a gear");
    if (gear != nullptr)
        gear->click();
    results.Expect(section == static_cast<int>(SettingsSection::Vision), "the gear reports the panel's Settings section");

    auto* beside = vision->findChild<QToolButton*>("panelPlaceBeside");
    if (beside != nullptr)
        beside->click();
    Settle();
    const BesidePlacement expected =
        PlaceBesideGeometry(owner.frameGeometry(), owner.screen()->availableGeometry(), Spec(PanelId::Vision).defaultSize.width(), 480);
    results.Expect(vision->isVisible() && FitsPlan(vision, expected.panelFrame) && owner.frameGeometry() == expected.ownerFrame,
        "Place beside chat moves the owner to the left edge and the panel beside it");

    host->SetTitleSuffix(PanelId::Canvas, " (3)");
    results.Expect(window->windowTitle() == "Canvas (3) — Revia" && host->Action(PanelId::Canvas)->text() == "Canvas (3)",
        "a title suffix reaches the window title and the action");
    host->Close(PanelId::Canvas);
    host->SetLocked(true);
    results.Expect(!page->isEnabled() && !window->findChild<QWidget*>("panelHeader")->isEnabled() && window->isEnabled(),
        "locking disables the panel's body but not the window");
    host->Action(PanelId::Canvas)->trigger();
    results.Expect(!host->Action(PanelId::Canvas)->isEnabled() && !host->IsOpen(PanelId::Canvas),
        "locking disables the panel actions, so their shortcuts open nothing");
    host->SetLocked(false);
    results.Expect(page->isEnabled() && host->Action(PanelId::Canvas)->isEnabled(), "unlocking enables the body and the actions again");

    host->Open(PanelId::Canvas);
    host->HideAll(true);
    results.Expect(!host->IsOpen(PanelId::Canvas) && !host->IsOpen(PanelId::Vision), "HideAll hides every panel");
    host->SaveOpenSet();
    results.Expect(QSettings().value("panels/openSet").toStringList() == QStringList{"canvas", "vision"},
        "SaveOpenSet counts the panels hidden to the tray");
    message->setFocus();
    Settle();
    host->RestoreHidden();
    Settle();
    results.Expect(host->IsOpen(PanelId::Canvas) && host->IsOpen(PanelId::Vision), "RestoreHidden reopens what HideAll hid");
    results.Expect(QApplication::activeWindow() == &owner && message->hasFocus(),
        "RestoreHidden leaves the owner active, with its focus widget, not the last panel shown");
    host->Close(PanelId::Vision);
    host->SaveOpenSet();
    results.Expect(QSettings().value("panels/openSet").toStringList() == QStringList{"canvas"}, "SaveOpenSet records the open panels");
    host->CloseAll();
    results.Expect(!host->IsOpen(PanelId::Canvas), "CloseAll closes every panel");
    host->RestoreOpenSet();
    Settle();
    results.Expect(host->IsOpen(PanelId::Canvas) && !host->IsOpen(PanelId::Vision), "RestoreOpenSet reopens the saved set");
    results.Expect(QApplication::activeWindow() == &owner, "RestoreOpenSet leaves the owner active");

    QAction* memory = host->Action(PanelId::Memory);
    results.Expect(memory != nullptr && owner.actions().contains(memory) && memory->shortcut() == QKeySequence("Ctrl+Shift+M") &&
                       memory->shortcutContext() == Qt::ApplicationShortcut && memory->isCheckable(),
        "panel actions sit on the owner with their application shortcuts");
    host->Close(PanelId::Canvas);
    host->Action(PanelId::Canvas)->trigger();
    results.Expect(host->IsOpen(PanelId::Canvas) && host->Action(PanelId::Canvas)->isChecked(), "triggering the action opens the panel");
    Settle();
    results.Expect(window->isActiveWindow(), "a panel opened by its action is the active window");
    host->Action(PanelId::Canvas)->trigger();
    results.Expect(!host->IsOpen(PanelId::Canvas) && !host->Action(PanelId::Canvas)->isChecked(),
        "triggering the action of the active panel closes it");
    host->Action(PanelId::Canvas)->trigger();
    ExpectListedNames(results, window, "a panel window");
    ExpectListedNames(results, vision, "a panel window with a gear");
    host->SetIconProvider([](const char*) { return Swatch(QColor("#57534E")); });
    Capture(window, "shell-panel-window.png");
    host->CloseAll();
    host.reset();
    results.Expect(guard && !owner.actions().contains(memory), "the host's actions leave with it; its windows stay with the owner");
}

// Placement works in frame rectangles, but QWidget::resize sizes the client area: the frame
// margins (2px a side even offscreen, a title bar on Windows) must come off, and Qt knows them
// only once the window shows.
void TestPanelFrames(Results& results)
{
    QSettings().remove("panels");
    QWidget owner;
    owner.setObjectName("testFramesOwner");
    owner.resize(760, 540);
    owner.move(0, 300);
    owner.show();
    Settle();
    const QRect available = owner.screen()->availableGeometry();
    PanelHost host(&owner);
    const auto standIn = [](const char* name)
    {
        auto* made = new QWidget;
        made->setObjectName(name);
        new QVBoxLayout(made);
        return made;
    };
    host.Adopt(PanelId::Voice, standIn("testVoicePage"));
    host.Adopt(PanelId::Memory, standIn("testMemoryPage"));
    PanelWindow* voice = host.Window(PanelId::Voice);
    PanelWindow* memory = host.Window(PanelId::Memory);

    // Voice is 700px tall: for an owner low on the screen it rises until its frame's bottom
    // meets the available area's.
    host.Open(PanelId::Voice);
    Settle();
    const QRect voiceFrame = voice->frameGeometry();
    results.Expect(available.contains(voiceFrame) && voiceFrame.bottom() == available.bottom(),
        "a tall panel's whole frame is clamped to the available area");

    // Memory never showed, so its rectangle waits for the show; Voice is showing and moves at once.
    const BesidePlacement memoryPlan = PlaceBesideGeometry(
        owner.frameGeometry(), available, Spec(PanelId::Memory).defaultSize.width(), Spec(PanelId::Memory).minUsefulWidth);
    host.PlaceBeside(PanelId::Memory);
    Settle();
    results.Expect(
        memory->isVisible() && memory->frameGeometry() == memoryPlan.panelFrame && owner.frameGeometry() == memoryPlan.ownerFrame,
        "Place beside chat gives a panel that never showed exactly the planned frame");
    const BesidePlacement voicePlan = PlaceBesideGeometry(
        owner.frameGeometry(), available, Spec(PanelId::Voice).defaultSize.width(), Spec(PanelId::Voice).minUsefulWidth);
    host.PlaceBeside(PanelId::Voice);
    Settle();
    results.Expect(voicePlan.panelFrame.width() < voice->minimumWidth() + voice->frameGeometry().width() - voice->width() &&
                       FitsPlan(voice, voicePlan.panelFrame) && voice->frameGeometry().right() == available.right(),
        "Place beside chat moves a showing panel at once; its minimum size grows it leftwards, not off the screen");
    host.CloseAll();
}

void TestSidebar(Results& results)
{
    int newChats = 0;
    int searches = 0;
    int panels = 0;
    int settings = 0;
    std::vector<std::pair<bool, bool>> toggles;
    ShellSidebar::Callbacks callbacks;
    callbacks.newChat = [&newChats]() { ++newChats; };
    callbacks.search = [&searches]() { ++searches; };
    callbacks.panels = [&panels]() { ++panels; };
    callbacks.settings = [&settings]() { ++settings; };
    callbacks.toggled = [&toggles](const bool expanded, const bool persisted) { toggles.emplace_back(expanded, persisted); };
    QWidget window;
    window.setObjectName("testSidebarWindow");
    window.resize(1000, 700);
    auto* row = new QHBoxLayout(&window);
    row->setContentsMargins(0, 0, 0, 0);
    auto* sidebar = new ShellSidebar(callbacks, &window);
    row->addWidget(sidebar);
    auto* chat = new QWidget(&window);
    chat->setObjectName("testChatColumn");
    row->addWidget(chat, 1);
    auto* body = new QWidget;
    body->setObjectName("testSidebarBody");
    sidebar->SetBody(body);
    QMenu menu(&window);
    menu.addAction("Settings…");
    sidebar->SetMenu(&menu);
    sidebar->SetAccount("Ada", "Default profile");
    sidebar->SetIconProvider([](const char*) { return Swatch(QColor("#57534E")); });
    window.show();
    Settle();

    ExpectPresent(results, sidebar,
        {"navigationSidebar", "sidebarHeader", "sidebarBrand", "sidebarBrandMark", "sidebarToggle", "sidebarBody", "sidebarRailNewChat",
            "sidebarRailSearch", "sidebarRailPanels", "sidebarFooter", "sidebarMenuButton", "sidebarAvatar", "sidebarAccountName",
            "sidebarAccountDetail", "sidebarAttentionDot", "sidebarSettingsButton"},
        "the sidebar");
    ExpectListedNames(results, sidebar, "the sidebar");
    results.Expect(sidebar->width() == 264 && sidebar->IsExpanded() && body->isVisible(), "the sidebar starts expanded, 264px wide");
    results.Expect(!sidebar->findChild<QToolButton*>("sidebarRailNewChat")->isVisible(), "the rail buttons hide while expanded");
    results.Expect(sidebar->findChild<QLabel*>("sidebarAvatar")->text() == "A", "the avatar shows the companion's initial");
    results.Expect(sidebar->findChild<QLabel*>("sidebarAccountName")->text().startsWith("Ad"), "the footer names the companion");
    const auto everyButtonNamed = [sidebar]()
    {
        for (const QToolButton* button : sidebar->findChildren<QToolButton*>())
        {
            if (button->isVisible() && (button->accessibleName().isEmpty() || button->toolTip().isEmpty()))
                return false;
        }
        return true;
    };
    results.Expect(everyButtonNamed(), "every visible sidebar button has an accessible name and a tooltip (expanded)");
    const auto sized = [sidebar](const std::vector<const char*>& buttons, const int side)
    {
        return std::all_of(buttons.begin(), buttons.end(),
            [sidebar, side](const char* button) { return sidebar->findChild<QToolButton*>(button)->size() == QSize(side, side); });
    };
    results.Expect(sized({"sidebarToggle", "sidebarSettingsButton"}, 32), "expanded, the header and footer icon buttons are 32x32");
    const auto tips = [sidebar](const char* button, const char* keys)
    { return sidebar->findChild<QToolButton*>(button)->toolTip().contains(QString::fromLatin1(keys)); };
    results.Expect(tips("sidebarToggle", "Ctrl+B") && tips("sidebarRailNewChat", "Ctrl+N") && tips("sidebarRailSearch", "Ctrl+F") &&
                       tips("sidebarSettingsButton", "Ctrl+,"),
        "the sidebar's tooltips name their shortcuts");
    const QList<QToolButton*> toolButtons = sidebar->findChildren<QToolButton*>();
    results.Expect(std::all_of(toolButtons.begin(), toolButtons.end(),
                       [](const QToolButton* button) { return button->focusPolicy() == Qt::TabFocus; }),
        "sidebar buttons take focus from the keyboard only");
    Capture(&window, "shell-sidebar-expanded.png");

    const int chatWidth = chat->width();
    sidebar->ToggleButton()->click();
    Settle();
    results.Expect(
        sidebar->width() == 56 && !sidebar->IsExpanded() && !body->isVisible(), "the toggle collapses the sidebar to a 56px rail");
    results.Expect(chat->width() > chatWidth, "the rail gives its width back to the chat");
    results.Expect(toggles.size() == 1 && toggles.back() == std::make_pair(false, true), "collapsing reports a persisted change");
    results.Expect(sidebar->findChild<QToolButton*>("sidebarRailNewChat")->isVisible() &&
                       sidebar->findChild<QToolButton*>("sidebarToggle")->isVisible(),
        "the rail shows its buttons and keeps the toggle");
    results.Expect(everyButtonNamed(), "every visible sidebar button has an accessible name and a tooltip (rail)");
    results.Expect(sized({"sidebarToggle", "sidebarRailNewChat", "sidebarRailSearch", "sidebarRailPanels", "sidebarSettingsButton",
                             "sidebarMenuButton"},
                       40),
        "the rail's buttons are 40x40");
    results.Expect(tips("sidebarToggle", "Ctrl+B"), "the rail's toggle names its shortcut");
    Capture(&window, "shell-sidebar-rail.png");
    sidebar->findChild<QToolButton*>("sidebarRailNewChat")->click();
    sidebar->findChild<QToolButton*>("sidebarRailSearch")->click();
    sidebar->findChild<QToolButton*>("sidebarRailPanels")->click();
    sidebar->findChild<QToolButton*>("sidebarSettingsButton")->click();
    results.Expect(newChats == 1 && searches == 1 && panels == 1 && settings == 1, "the rail and footer buttons call back");
    sidebar->ToggleButton()->click();
    Settle();
    results.Expect(sidebar->width() == 264 && toggles.back() == std::make_pair(true, true), "the toggle expands it to 264px again");

    sidebar->SetNarrow(true);
    Settle();
    results.Expect(sidebar->width() == 56 && !sidebar->IsExpanded(), "a narrow window forces the rail");
    sidebar->ToggleButton()->click();
    results.Expect(sidebar->width() == 264 && toggles.back() == std::make_pair(true, false), "expanding a narrow window is not persisted");
    sidebar->SetNarrow(false);
    results.Expect(sidebar->width() == 264 && sidebar->IsExpanded(), "a wide window returns to the preference");
    sidebar->SetNarrow(true);
    results.Expect(sidebar->width() == 56, "narrowing again forgets the temporary expansion");
    sidebar->SetNarrow(false);
    const std::size_t toggleCount = toggles.size();
    sidebar->SetExpanded(false);
    results.Expect(sidebar->width() == 56 && toggles.size() == toggleCount, "SetExpanded applies the preference without reporting it");
    sidebar->SetExpanded(true);

    auto* dot = sidebar->findChild<QLabel*>("sidebarAttentionDot");
    results.Expect(!dot->isVisible(), "the attention dot hides while Activity is clear");
    sidebar->SetAttention(true);
    results.Expect(dot->isVisible() && dot->property("attention").toBool(), "the attention dot shows warnings");
    Settle(); // the footer's layout after the mode changes above
    sidebar->PopupMenu();
    Settle();
    const QPoint menuButtonTop = sidebar->findChild<QToolButton*>("sidebarMenuButton")->mapToGlobal(QPoint(0, 0));
    results.Expect(menu.isVisible() && menu.geometry().bottom() < menuButtonTop.y() && menu.x() == menuButtonTop.x(),
        "the menu button opens the menu above itself");
    menu.hide();

    // Without a panels callback the rail button opens the menu beside itself, the anchor action
    // level with the button.
    ShellSidebar::Callbacks bare;
    ShellSidebar railOnly(bare);
    QAction* canvas = menu.addAction("Canvas");
    railOnly.SetMenu(&menu);
    railOnly.SetPanelsAnchor(canvas);
    railOnly.SetExpanded(false);
    railOnly.resize(ShellSidebar::RailWidth, 420);
    railOnly.move(40, 60);
    railOnly.show();
    Settle();
    auto* railPanels = railOnly.findChild<QToolButton*>("sidebarRailPanels");
    railPanels->click();
    Settle();
    const QPoint buttonTop = railPanels->mapToGlobal(QPoint(railPanels->width(), 0));
    results.Expect(
        menu.isVisible() && menu.x() >= buttonTop.x() && std::abs(menu.y() + menu.actionGeometry(canvas).top() - buttonTop.y()) <= 2,
        "the rail's panels button opens the menu beside it at the anchor action");
    menu.hide();
}

void TestComposer(Results& results)
{
    QWidget window;
    window.setObjectName("testComposerWindow");
    window.resize(720, 360);
    auto* column = new QVBoxLayout(&window);
    column->addStretch(1);
    auto* shadow = new SoftShadowHost(&window);
    shadow->setObjectName("composerShadowHost");
    auto* card = new ComposerCard;
    shadow->SetTarget(card);
    shadow->SetRadius(20);
    column->addWidget(shadow);
    auto* input = new QPlainTextEdit;
    input->setObjectName("testMessageInput");
    input->setPlaceholderText("Message Ada");
    auto* tray = new QWidget;
    tray->setObjectName("testTray");
    tray->setFixedHeight(10);
    card->SetInput(input);
    card->SetTray(tray);
    const auto button = [](const char* name, const QString& text)
    {
        auto* made = new QPushButton(text);
        made->setObjectName(name);
        made->setFixedSize(32, 32);
        return made;
    };
    QPushButton* attach = button("testAttach", "+");
    QPushButton* microphone = button("testMicrophone", "M");
    QPushButton* stop = button("testStop", "■");
    QPushButton* send = button("testSend", "↑");
    auto* health = new QLabel("Text mode");
    health->setObjectName("testVoiceHealth");
    card->AddLeading(attach);
    card->AddTrailing(microphone);
    card->AddTrailing(send);
    card->InsertTrailing(card->TrailingIndexOf(send), stop);
    card->InsertTrailing(card->TrailingIndexOf(microphone), health);
    window.show();
    Settle();

    results.Expect(card->objectName() == "composerCard" && shadow->Target() == card, "the card sits in its shadow host");
    ExpectPresent(results, card, {"composerCard", "composerContext", "composerContextLabel", "composerContextAction", "composerToolbar"},
        "the composer");
    ExpectListedNames(results, card, "the composer");
    results.Expect(card->TrailingIndexOf(health) == 0 && card->TrailingIndexOf(microphone) == 1 && card->TrailingIndexOf(stop) == 2 &&
                       card->TrailingIndexOf(send) == 3,
        "InsertTrailing places widgets before the one it is given");
    results.Expect(attach->parentWidget() != nullptr && attach->parentWidget()->objectName() == "composerToolbar" &&
                       attach->x() < microphone->x() && microphone->x() < send->x(),
        "the toolbar holds leading widgets left and trailing widgets right");
    results.Expect(tray->y() < input->y() && input->y() < attach->parentWidget()->y(), "tray, then message box, then toolbar");

    // Plain text lines are whole pixels tall; allow a pixel either way for the font's metrics.
    const qreal line = std::ceil(QFontMetricsF(input->font()).lineSpacing());
    const auto near = [](const int actual, const qreal expected) { return std::abs(actual - expected) <= 1.0; };
    const int one = input->height();
    results.Expect(input->verticalScrollBar()->maximum() == 0, "one line fits without scrolling");
    input->setPlainText("one\ntwo\nthree");
    Settle();
    const int three = input->height();
    results.Expect(near(three, one + 2 * line) && input->verticalScrollBar()->maximum() == 0, "the message box grows a line at a time");
    input->setPlainText(QString("line\n").repeated(30));
    Settle();
    results.Expect(near(input->height(), std::min<qreal>(one + 7 * line, 200)), "the message box stops at eight lines or 200px");
    results.Expect(input->verticalScrollBar()->maximum() > 0, "beyond the limit the message box scrolls");
    card->SetMaxInputHeight(100);
    results.Expect(input->height() <= 100, "the message box respects the height limit");
    card->SetMaxInputLines(2);
    results.Expect(near(input->height(), std::min<qreal>(one + line, 100)), "the message box respects the line limit");
    card->SetMaxInputLines(8);
    card->SetMaxInputHeight(200);
    input->clear();
    Settle();
    results.Expect(input->height() == one, "an empty message box is one line tall");
    results.Expect(input->tabChangesFocus() && input->frameShape() == QFrame::NoFrame, "the message box is frameless and Tab moves on");

    auto* context = card->findChild<QWidget*>("composerContext");
    auto* contextLabel = card->findChild<QLabel*>("composerContextLabel");
    auto* contextAction = card->findChild<QPushButton*>("composerContextAction");
    results.Expect(!context->isVisible(), "the context row hides when there is no mode");
    int cancelled = 0;
    card->SetContext("Editing your last message", "Cancel (Esc)",
        [&cancelled, card]()
        {
            ++cancelled;
            card->ClearContext();
        });
    Settle();
    results.Expect(context->isVisible() && contextLabel->text() == "Editing your last message" && contextAction->isVisible() &&
                       contextAction->text() == "Cancel (Esc)",
        "the context row shows the mode and its action");
    Capture(&window, "shell-composer-context.png");
    contextAction->click();
    results.Expect(cancelled == 1 && !context->isVisible(), "the context action runs and may clear the row");
    card->SetContext("Queued until the current studio operation finishes", {}, {});
    results.Expect(context->isVisible() && !contextAction->isVisible(), "a context without an action has no button");
    card->ClearContext();

    QFocusEvent focusIn(QEvent::FocusIn, Qt::TabFocusReason);
    QCoreApplication::sendEvent(input, &focusIn);
    results.Expect(card->property("focused").toBool(), "the card is marked focused with its message box");
    QFocusEvent focusOut(QEvent::FocusOut, Qt::TabFocusReason);
    QCoreApplication::sendEvent(input, &focusOut);
    results.Expect(!card->property("focused").toBool(), "the focus mark clears with the message box");

    card->SetVoiceState("listening");
    results.Expect(card->property("voice").toString() == "listening" && input->placeholderText() == "Listening…",
        "listening shows in the voice property and the placeholder");
    card->SetVoiceState("transcribing");
    results.Expect(input->placeholderText() == "Transcribing…", "transcribing has its own placeholder");
    card->SetVoiceState("idle");
    results.Expect(input->placeholderText() == "Message Ada", "the placeholder comes back after listening");
    card->SetVoiceState("listening");
    card->SetPlaceholder("Message Bo");
    results.Expect(input->placeholderText() == "Listening…", "a new placeholder waits while listening");
    card->SetVoiceState("handsfree");
    results.Expect(
        input->placeholderText() == "Message Bo" && card->VoiceState() == "handsfree", "hands-free keeps the normal placeholder");
    card->SetVoiceState("idle");
    input->setPlainText("Could you summarise the meeting notes from this morning?");
    Settle();
    Capture(&window, "shell-composer.png");

    // The host paints a shadow around the card: just below it is darker than the far corner.
    QPalette shadowPalette = shadow->palette();
    shadowPalette.setColor(QPalette::Window, Qt::white);
    shadowPalette.setColor(QPalette::Shadow, QColor(0, 0, 0, 120));
    shadow->setPalette(shadowPalette);
    shadow->setAutoFillBackground(true);
    shadow->SetBlur(12);
    shadow->SetOffset(QPoint(0, 4));
    Settle();
    const QImage painted = shadow->grab().toImage();
    const QRect cardRect = card->geometry();
    const QColor below = painted.pixelColor(cardRect.center().x(), cardRect.bottom() + 5);
    const QColor corner = painted.pixelColor(1, 1);
    results.Expect(below.lightness() < corner.lightness() - 8, "the shadow host darkens the area just under its target");
    results.Expect(corner.lightness() > 250, "the shadow fades out before the host's corner");
    results.Expect(shadow->ShadowColour().alpha() == 120, "a translucent palette Shadow is used as it is");
    shadowPalette.setColor(QPalette::Shadow, Qt::black);
    shadow->setPalette(shadowPalette);
    results.Expect(shadow->ShadowColour().alpha() < 64, "an opaque palette Shadow is softened");
    QImage canvas(40, 40, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);
    {
        QPainter painter(&canvas);
        PaintSoftShadow(painter, QRect(10, 10, 6, 6), 20, 16, QPoint(0, 2), QColor(0, 0, 0, 90));
        PaintSoftShadow(painter, QRect(20, 20, 10, 10), 4, 0, QPoint(), QColor(0, 0, 0, 255));
        PaintSoftShadow(painter, QRect(), 4, 8, QPoint(), QColor(0, 0, 0, 255));
    }
    results.Expect(canvas.pixelColor(25, 25) == QColor(Qt::black) && canvas.pixelColor(13, 14).lightness() < 255,
        "PaintSoftShadow handles no blur and targets smaller than its corners");
}

void TestToast(Results& results)
{
    QWidget host;
    host.setObjectName("testToastHost");
    host.resize(700, 500);
    auto* anchor = new QWidget(&host);
    anchor->setObjectName("testAnchor");
    anchor->setGeometry(100, 400, 500, 80);
    auto* toast = new ToastOverlay(&host);
    toast->SetAnchor(anchor);
    int dismissed = 0;
    toast->SetDismissHook([&dismissed]() { ++dismissed; });
    host.show();
    Settle();
    auto* label = toast->findChild<QLabel*>("toastLabel");
    results.Expect(toast->objectName() == "toast" && label != nullptr && !toast->isVisible(), "the toast starts hidden");
    if (label == nullptr)
        return;
    ExpectListedNames(results, toast, "the toast");

    toast->ShowMessage("Saved", 3000);
    Settle();
    results.Expect(toast->isVisible() && label->text() == "Saved" && toast->HideDelayMs() == 3000, "a message shows for its timeout");
    results.Expect(toast->geometry().bottom() < anchor->geometry().top() &&
                       std::abs(toast->geometry().center().x() - anchor->geometry().center().x()) <= 1,
        "the toast sits centred above its anchor");
    Capture(&host, "shell-toast.png");
    toast->ShowMessage("Long", 60000);
    results.Expect(toast->HideDelayMs() == ToastOverlay::MaximumTimeoutMs, "a toast never lasts longer than 5 s");
    toast->ShowMessage("Sticky", 0);
    results.Expect(toast->HideDelayMs() == ToastOverlay::MaximumTimeoutMs, "a message without a timeout still goes after 5 s");
    const QString longText = QString("This status line is far too long to fit on a single toast line. ").repeated(5);
    toast->ShowMessage(longText, 3000);
    Settle();
    results.Expect(toast->width() <= ToastOverlay::MaximumWidth && label->text() != longText && label->text().endsWith("…") &&
                       toast->toolTip() == longText,
        "a long message is elided to one line, in full in the tooltip");
    toast->ShowMessage("First", 3000);
    toast->ShowMessage("Second", 3000);
    results.Expect(toast->Message() == "Second" && label->text() == "Second", "a new message replaces the old one");

    toast->ShowMessage("Briefly", 120);
    Wait(400);
    results.Expect(!toast->isVisible() && dismissed == 0, "the toast hides after its timeout without dismissing the message");
    toast->ShowMessage("Click to dismiss", 3000);
    Settle();
    Press(toast, QPoint(4, 4));
    Settle();
    results.Expect(!toast->isVisible() && dismissed == 1, "a click hides the toast and dismisses the message");
    toast->ShowMessage("Going", 3000);
    toast->ShowMessage({}, 3000);
    results.Expect(!toast->isVisible() && toast->Message().isEmpty(), "an empty message clears the toast");

    // Following the window's status bar: what reaches the bar toasts, and the bar stays the record.
    auto* bar = new QStatusBar(&host);
    bar->setObjectName("testStatusBar");
    toast->Follow(bar);
    bar->showMessage("toast fixture");
    results.Expect(toast->isVisible() && toast->Message() == "toast fixture" && toast->HideDelayMs() == ToastOverlay::MaximumTimeoutMs,
        "a message put straight on the bar toasts for at most 5 s");
    toast->ShowStatus("Saved", 3000);
    results.Expect(bar->currentMessage() == "Saved" && toast->Message() == "Saved" && toast->HideDelayMs() == 3000,
        "ShowStatus puts the message on the bar and toasts it for the bar's timeout");
    toast->ShowStatus("Revia is still working on the previous request", 8000);
    toast->Clear(); // as when its 5 s are up: the bar keeps the message for its own 8 s
    toast->ShowStatus("Revia is still working on the previous request", 8000);
    results.Expect(toast->isVisible() && bar->currentMessage() == "Revia is still working on the previous request",
        "a message repeated while the bar still shows it toasts again");
    Press(toast, QPoint(4, 4));
    results.Expect(!toast->isVisible() && bar->currentMessage().isEmpty(), "a click dismisses the toast and clears the bar");
    bar->showMessage("Brief", 100);
    results.Expect(toast->isVisible(), "a timed bar message toasts");
    Wait(300);
    results.Expect(!toast->isVisible() && bar->currentMessage().isEmpty(), "the toast goes when the bar's message times out");
}

void TestShortcutSheet(Results& results)
{
    QWidget owner;
    auto* sheet = new ShortcutSheet({{"Window", ""}, {"New chat", "Ctrl+N"}, {"Toggle sidebar", "Ctrl+B"}}, &owner);
    auto* table = sheet->findChild<QTableWidget*>("shortcutsTable");
    results.Expect(sheet->objectName() == "shortcutsDialog" && !sheet->isModal() && sheet->size() == QSize(440, 520),
        "the shortcut sheet is a modeless 440x520 dialog");
    results.Expect(table != nullptr && table->rowCount() == 3 && table->columnCount() == 2, "the sheet's table has a row per entry");
    if (table != nullptr)
    {
        results.Expect(table->item(1, 0)->text() == "New chat" && table->item(1, 1)->text() == "Ctrl+N", "rows read action, then keys");
        results.Expect(table->columnSpan(0, 0) == 2 && table->item(0, 1) == nullptr, "a row without keys is a heading across the table");
        results.Expect(table->editTriggers() == QAbstractItemView::NoEditTriggers, "the table is read-only");
    }
    ExpectListedNames(results, sheet, "the shortcut sheet");
    sheet->show();
    Settle();
    Capture(sheet, "shell-shortcuts.png");
    sheet->close();
}

void CapturePaletteGallery()
{
    if (qEnvironmentVariable("REVIA_UI_RENDER_DIR").isEmpty())
        return;
    QSettings().setValue("palette/recent", QStringList{"panel:memory"});
    QWidget host;
    host.setObjectName("testGallery");
    host.resize(960, 640);
    host.setAutoFillBackground(true);
    host.show();
    auto* palette = new CommandPalette(&host);
    std::vector<CommandEntry> commands;
    const auto add = [&commands](const QString& id, const QString& title, const QString& group, const QString& subtitle,
                         const QString& keys, const QColor& colour)
    {
        CommandEntry command;
        command.id = id;
        command.title = title;
        command.group = group;
        command.subtitle = subtitle;
        command.shortcutText = keys;
        command.icon = Swatch(colour);
        command.run = []() {};
        commands.push_back(command);
    };
    const QColor tone("#57534E");
    add("action:newChat", "New chat", "Actions", {}, "Ctrl+N", tone);
    add("action:sidebar", "Toggle sidebar", "Actions", {}, "Ctrl+B", tone);
    add("action:settings", "Settings", "Actions", {}, "Ctrl+,", tone);
    add("panel:memory", "Open Memory", "Panels", "Window", "Ctrl+Shift+M", tone);
    add("panel:activity", "Open Activity", "Panels", "Window", "Ctrl+Shift+A", tone);
    add("panel:voice", "Open Voice", "Panels", "Window", {}, tone);
    add("setting:speech", "Speak replies", "Settings", "Settings › Voice", {}, tone);
    palette->SetCommands(commands);
    palette->Open();
    Settle();
    Capture(&host, "shell-palette.png");
    palette->Open("mem");
    Settle();
    Capture(&host, "shell-palette-query.png");
    palette->Close();
    QSettings().remove("palette/recent");
}

int RunShellSuite(int, char**)
{
    Results results;
    TestFuzzyScore(results);
    TestPalette(results);
    TestPlacement(results);
    TestPanelCatalog(results);
    TestPanelWindows(results);
    TestPanelFrames(results);
    TestSidebar(results);
    TestComposer(results);
    TestToast(results);
    TestShortcutSheet(results);
    CapturePaletteGallery();
    if (results.failures == 0)
        std::cout << "Shell widget checks passed.\n";
    return results.failures;
}
}

REVIA_DESKTOP_SUITE("shell", RunShellSuite);
