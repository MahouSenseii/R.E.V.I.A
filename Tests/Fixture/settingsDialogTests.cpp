#include "capabilityPanel.h"
#include "desktopObjectNames.h"
#include "desktopSuites.h"
#include "profilePanel.h"
#include "settingsCatalog.h"
#include "settingsDialog.h"
#include "reviaSessionTestAccess.h"

#include "Actions/actionTypes.h"
#include "Core/preferenceStore.h"
#include "Core/settingsFileKeys.h"
#include "Runtime/reviaSession.h"

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QWheelEvent>
#include <QComboBox>
#include <QIcon>
#include <QPalette>
#include <QPixmap>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Revia.SettingsDialog: the settings catalog against the settings inventory, and the Settings
// dialog, CapabilityPanel and ProfilePanel behaviour, all through a fake backend whose writes
// finish synchronously unless told to wait. Nothing here writes a preference, the owner settings
// overlay or the virtual machine file. The only real session is never started; the capability
// switches it backs are saved to a fixture capability file in the temporary root.
namespace
{
using namespace revia::desktop;

struct Expect
{
    int failures = 0;
    void operator()(const bool ok, const QString& what)
    {
        if (!ok)
        {
            ++failures;
            std::cout << "FAIL: " << what.toStdString() << '\n';
        }
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

QString Q(const std::string& text)
{
    return QString::fromStdString(text);
}

// The 84 user-facing booleans of the settings inventory (design work for the redesign,
// inventory.json "userFacing"), each with the catalog key that carries it.
const std::vector<std::pair<std::string, std::string>> kInventory = {
    {"window/alwaysOnTop (QSettings)", "window/alwaysOnTop"},
    {"chat/sidebarVisible (QSettings)", "chat/sidebarVisible"},
    {"activity/follow (QSettings)", "activity/follow"},
    {"input/autoSendVoice (QSettings)", "input/autoSendVoice"},
    {"speech.enabled", "speech.enabled"},
    {"speech.speakGreeting", "speech.speakGreeting"},
    {"bargeIn.enabled", "bargeIn.enabled"},
    {"speechRecognition.enabled", "speechRecognition.enabled"},
    {"speechRecognition.handsFree", "speechRecognition.handsFree"},
    {"speechRecognition.requireWakeWord", "speechRecognition.requireWakeWord"},
    {"initiative.enabled", "initiative.enabled"},
    {"initiative.curiosityEnabled", "initiative.curiosityEnabled"},
    {"initiative.spontaneousSpeechEnabled", "initiative.spontaneousSpeechEnabled"},
    {"initiative.speakWhenUserAway", "initiative.speakWhenUserAway"},
    {"initiative.suppressWhenFullScreen", "initiative.suppressWhenFullScreen"},
    {"initiative.autonomousLearningEnabled", "initiative.autonomousLearningEnabled"},
    {"conversation.archiveEnabled", "conversation.archiveEnabled"},
    {"conversation.selfInquiryEnabled", "conversation.selfInquiryEnabled"},
    {"conversation.iterativeInvestigationEnabled", "conversation.iterativeInvestigationEnabled"},
    {"responseFilter.aiReviewEnabled", "responseFilter.aiReviewEnabled"},
    {"performance.enabled", "performance.enabled"},
    {"performance.interruptSongToSpeak", "performance.interruptSongToSpeak"},
    {"profile.memoryEnabled", "profile.memoryEnabled"},
    {"profile.temperature present (override)", "profile.hasTemperatureOverride"},
    {"profile.maxTokens present (override)", "profile.hasMaxTokensOverride"},
    {"vision.enabled", "vision.enabled"},
    {"vision.continuousAwareness", "vision.continuousAwareness"},
    {"perception.enabled", "perception.enabled"},
    {"session: perception.paused", "perception.paused"},
    {"llm.visionEnabled", "llm.visionEnabled"},
    {"capabilities.internet.enabled", "capabilities.internet.enabled"},
    {"capabilities.internet.automaticLookup", "capabilities.internet.automaticLookup"},
    {"capabilities.internet.visibleBrowser", "capabilities.internet.visibleBrowser"},
    {"capabilities.internet.autonomousResearch", "capabilities.internet.autonomousResearch"},
    {"capabilities.browser.enabled", "capabilities.browser.enabled"},
    {"capabilities.browser.navigate", "capabilities.browser.navigate"},
    {"capabilities.browser.interact", "capabilities.browser.interact"},
    {"capabilities.browser.allowTaskInteraction", "capabilities.browser.allowTaskInteraction"},
    {"capabilities.browser.allowLoopback", "capabilities.browser.allowLoopback"},
    {"capabilities.camera.enabled", "capabilities.camera.enabled"},
    {"capabilities.camera.autonomousCapture", "capabilities.camera.autonomousCapture"},
    {"capabilities.desktopControl.pointer", "capabilities.desktopControl.pointer"},
    {"capabilities.desktopControl.keyboard", "capabilities.desktopControl.keyboard"},
    {"capabilities.desktopControl.applicationLaunch", "capabilities.desktopControl.applicationLaunch"},
    {"capabilities.desktopControl.rawCoordinates", "capabilities.desktopControl.rawCoordinates"},
    {"capabilities.desktopControl.visualTargeting", "capabilities.desktopControl.visualTargeting"},
    {"capabilities.desktopControl.scope == whole_desktop", "capabilities.desktopControl.wholeDesktop"},
    {"capabilities.desktopControl.allowCommandSurfaces", "capabilities.desktopControl.allowCommandSurfaces"},
    {"capabilities.desktopControl.autonomous", "capabilities.desktopControl.autonomous"},
    {"capabilities.process.enabled", "capabilities.process.enabled"},
    {"capabilities.process.allowTaskExecution", "capabilities.process.allowTaskExecution"},
    {"capabilities.process.allowCommandInterpreters", "capabilities.process.allowCommandInterpreters"},
    {"capabilities.guest.enabled", "capabilities.guest.enabled"},
    {"capabilities.guest.read", "capabilities.guest.read"},
    {"capabilities.guest.write", "capabilities.guest.write"},
    {"capabilities.guest.execute", "capabilities.guest.execute"},
    {"capabilities.guest.allowTaskExecution", "capabilities.guest.allowTaskExecution"},
    {"RuntimeData/VirtualMachine/settings.json: enabled", "virtualMachine.enabled"},
    {"intelligence.enabled", "intelligence.enabled"},
    {"intelligence.fast.enabled", "intelligence.fast.enabled"},
    {"intelligence.expert.enabled", "intelligence.expert.enabled"},
    {"intelligence.expert.onDemand", "intelligence.expert.onDemand"},
    {"llm.cloud.enabled", "llm.cloud.enabled"},
    {"session: cloud session gate (/cloud on|off)", "cloud.session"},
    {"image.enabled", "image.enabled"},
    {"image.keepLoaded", "image.keepLoaded"},
    {"image.offline", "image.offline"},
    {"embedding.enabled", "embedding.enabled"},
    {"presence.enabled", "presence.enabled"},
    {"presence.avatarBridgeEnabled", "presence.avatarBridgeEnabled"},
    {"presence.externalAdaptersEnabled", "presence.externalAdaptersEnabled"},
    {"presence.requireAddressedStreamMessages", "presence.requireAddressedStreamMessages"},
    {"presence.speakStreamReplies", "presence.speakStreamReplies"},
    {"localApi.enabled", "localApi.enabled"},
    {"localApi.trustedPrivate", "localApi.trustedPrivate"},
    {"mcp.servers[].enabled", "mcp.servers[*].enabled"},
    {"mcp.servers[].requireConfirmation", "mcp.servers[*].requireConfirmation"},
    {"improvement.enabled", "improvement.enabled"},
    {"improvement.explore", "improvement.explore"},
    {"improvement.verify", "improvement.verify"},
    {"llm.autoTune", "llm.autoTune"},
    {"llm.autoMaxTokens", "llm.autoMaxTokens"},
    {"resources.autoPlan", "resources.autoPlan"},
    {"resources.allowChatModelSplit", "resources.allowChatModelSplit"},
};

// Rows the redesign adds beyond the inventory: new shell preferences, and booleans the inventory
// had excluded that the design spec turned back into switches.
const std::set<std::string> kAdditions = {"window/closeToTray", "sidebar/expanded", "panels/restoreOpen",
    "chat/showWorkSummaries", "intelligence.fast.warmAtStartup", "intelligence.expert.warmAtStartup", "llm.autoStartServer",
    "embedding.autoStartServer", "speechRecognition.useGpu"};

const QRegularExpression kGenderedWords(QStringLiteral("\\b(she|her|herself|hers)\\b"), QRegularExpression::CaseInsensitiveOption);

// The catalog row a key belongs to, tool-server rows mapped back to their template.
const SettingDescriptor* CatalogRow(const std::string& key)
{
    if (key.rfind("mcp.servers[", 0) == 0)
    {
        const auto close = key.find(']');
        return FindSetting("mcp.servers[*]" + key.substr(close + 1));
    }
    return FindSetting(key);
}

struct FakeBackend
{
    std::map<std::string, SettingRead> state;
    std::vector<std::pair<std::string, bool>> writes;
    std::set<std::string> refused;
    bool deferNext = false;
    SettingDone deferred;
    std::vector<std::string> servers{"files", "web-search"};
    std::map<std::string, QString> choices;
    std::map<std::string, QString> status;
    std::vector<std::pair<std::string, double>> profileWrites;
    std::map<std::string, double> profileValues{{"temperature", 0.75}, {"maxTokens", 640.0}};
    bool refuseProfileWrites = false;
    bool deferNextProfileWrite = false;
    SettingDone deferredProfileWrite;
    std::vector<std::string> resets;
    // Where a reset leads, when it is not the catalog default (an owner overlay key falls back to
    // whatever Config/settings.json ships).
    std::map<std::string, bool> resetTo;

    SettingRead& Entry(const std::string& key)
    {
        auto found = state.find(key);
        if (found == state.end())
        {
            const SettingDescriptor* row = CatalogRow(key);
            const bool value = row != nullptr && row->defaultValue;
            found = state.emplace(key, SettingRead{true, value, value, {}}).first;
        }
        return found->second;
    }

    void Set(const std::string& key, const bool value)
    {
        SettingRead& entry = Entry(key);
        entry.value = value;
        entry.running = value;
    }

    SettingsBackend Make()
    {
        SettingsBackend backend;
        backend.readAll = [this](const std::vector<SettingKeyRef>& keys)
        {
            std::map<std::string, SettingRead> values;
            for (const SettingKeyRef& key : keys)
                values[key.key] = Entry(key.key);
            return values;
        };
        backend.write = [this](const SettingKeyRef& key, const bool value, SettingDone done)
        {
            writes.emplace_back(key.key, value);
            if (refused.contains(key.key))
            {
                done({false, QStringLiteral("Refused by the fake backend.")});
                return;
            }
            SettingRead& entry = Entry(key.key);
            entry.value = value;
            // Like the session: Live keys are committed at once and an off-live row commits only
            // turning off. A Partial key reports what Start read as running, like a restart-only
            // one, because only part of it follows the change before a restart.
            const SettingDescriptor* row = CatalogRow(key.key);
            if (row == nullptr || row->apply == SettingApply::Live || (row->apply == SettingApply::LiveWhenOffRestartWhenOn && !value))
                entry.running = value;
            if (deferNext)
            {
                deferNext = false;
                deferred = std::move(done);
                return;
            }
            done({true, QStringLiteral("Saved.")});
        };
        backend.reset = [this](const SettingKeyRef& key, SettingDone done)
        {
            resets.push_back(key.key);
            const SettingDescriptor* row = CatalogRow(key.key);
            const auto target = resetTo.find(key.key);
            Set(key.key, target != resetTo.end() ? target->second : row != nullptr && row->defaultValue);
            done({true, QStringLiteral("Back to the default.")});
        };
        backend.readChoice = [this](const std::string& key) { return choices.contains(key) ? choices[key] : QString(); };
        backend.writeChoice = [this](const std::string& key, const QString& value, SettingDone done)
        {
            choices[key] = value;
            done({true, QString()});
        };
        backend.readProfileValue = [this](const std::string& field) { return profileValues[field]; };
        backend.writeProfileValue = [this](const std::string& field, const double value, SettingDone done)
        {
            profileWrites.emplace_back(field, value);
            if (refuseProfileWrites)
            {
                // What B1's adapter says while the Profiles editor holds unsaved edits.
                done({false, QStringLiteral("Save or discard your changes in Profiles first.")});
                return;
            }
            profileValues[field] = value;
            if (deferNextProfileWrite)
            {
                deferNextProfileWrite = false;
                deferredProfileWrite = std::move(done);
                return;
            }
            done({true, QStringLiteral("Profile saved.")});
        };
        backend.mcpServers = [this]() { return servers; };
        backend.statusText = [this](const std::string& key) { return status.contains(key) ? status[key] : QString(); };
        return backend;
    }
};

QMessageBox* OpenConfirmation(QWidget* root)
{
    Settle();
    for (auto* box : root->findChildren<QMessageBox*>(QStringLiteral("settingConfirmDialog")))
    {
        if (box->isVisible())
            return box;
    }
    return nullptr;
}

QAbstractButton* ButtonWithText(QMessageBox* box, const QString& text)
{
    for (QAbstractButton* button : box->buttons())
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

QLabel* RowLabel(QWidget* root, const QString& rowName, const char* labelName)
{
    auto* row = root->findChild<QFrame*>(rowName);
    return row != nullptr ? row->findChild<QLabel*>(QString::fromUtf8(labelName)) : nullptr;
}

QLabel* Badge(QWidget* row, const QString& kind)
{
    for (QLabel* badge : row->findChildren<QLabel*>(QStringLiteral("settingBadge")))
    {
        if (badge->property("kind").toString() == kind)
            return badge;
    }
    return nullptr;
}

void PressKey(QWidget* target, const int key, const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    // A key press that is not spontaneous is offered to shortcuts first, as a typed one is.
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(target, &release);
    Settle();
}

// A left click in the middle of a widget; a label passes it on to the row it sits in.
void ClickOn(QWidget* target)
{
    if (target == nullptr)
        return;
    const QPointF centre = QRectF(target->rect()).center();
    const QPointF global = target->mapToGlobal(centre);
    QMouseEvent press(QEvent::MouseButtonPress, centre, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, centre, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &release);
    Settle();
}

// The box holding a group: its title label and its frame of rows.
QWidget* GroupBox(QWidget* page, const QString& title)
{
    for (QLabel* label : page->findChildren<QLabel*>(QStringLiteral("settingsGroupTitle")))
    {
        if (label->text() == title)
            return label->parentWidget();
    }
    return nullptr;
}

// One notch of the mouse wheel over a widget; true when the widget took it. A synthetic event is not
// passed on to the parents the way a real one is, so a guarded control simply leaves it unaccepted.
bool WheelTaken(QWidget* target)
{
    const QPointF centre = QRectF(target->rect()).center();
    QWheelEvent wheel(centre, target->mapToGlobal(centre), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(target, &wheel);
    Settle();
    return wheel.isAccepted();
}

// Opens a row's context menu the way a right-click does and reports whether "Reset to default" was
// there and enabled, choosing it when asked to. QMenu::exec runs its own loop, so the menu is
// examined and closed from a timer inside it.
bool ResetOffered(QWidget* row, const bool choose)
{
    auto offered = std::make_shared<bool>(false);
    QTimer::singleShot(0, row, [offered, choose]()
    {
        auto* menu = dynamic_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr)
            return;
        for (QAction* action : menu->actions())
        {
            if (action->text() == "Reset to default")
            {
                *offered = action->isEnabled();
                if (*offered && choose)
                {
                    menu->setActiveAction(action);
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(menu, &press);
                }
            }
        }
        if (menu->isVisible())
            menu->close();
    });
    const QPoint inside = row->rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, inside, row->mapToGlobal(inside));
    QApplication::sendEvent(row, &event);
    Settle();
    return *offered;
}

void CheckCatalog(Expect& expect)
{
    const auto& catalog = SettingCatalog();
    std::map<std::string, int> keyCounts;
    std::set<std::string> objectNames;
    for (const SettingDescriptor& row : catalog)
    {
        ++keyCounts[row.key];
        expect(objectNames.insert(row.objectName).second, "objectName " + Q(row.objectName) + " must be unique");
        expect(!row.label.isEmpty() && !row.description.isEmpty() && !row.group.isEmpty(),
            Q(row.key) + " needs a label, a one-line description and a group");
        if (row.risk == SettingRisk::Dangerous)
            expect(row.confirm != ConfirmWhen::Never, Q(row.key) + " is risky and must ask before it changes");
        if (row.confirm != ConfirmWhen::Never)
            expect(!row.warning.isEmpty(), Q(row.key) + " asks for confirmation and needs a warning to show");
        for (const QString& text : {row.label, row.description, row.warning})
            expect(!text.contains(kGenderedWords), Q(row.key) + " must use {name}, not a gendered pronoun: " + text);
        for (const std::string& required : row.requiresAll)
            expect(CatalogRow(required) != nullptr, Q(row.key) + " requires an unknown key " + Q(required));
        if (row.host == SettingHost::Dialog)
            expect(row.objectName.rfind("sw_", 0) == 0 || row.objectName.ends_with("Check"),
                Q(row.objectName) + " should be an sw_ name or an existing *Check name");
    }
    for (const auto& [key, count] : keyCounts)
        expect(count == 1, Q(key) + " appears " + QString::number(count) + " times in the catalog");

    // Every one of the inventory's 84 booleans is a row exactly once.
    std::set<std::string> inventoryKeys;
    std::set<std::string> mapped;
    for (const auto& [inventoryKey, catalogKey] : kInventory)
    {
        inventoryKeys.insert(inventoryKey);
        mapped.insert(catalogKey);
        expect(keyCounts[catalogKey] == 1, "inventory boolean " + Q(inventoryKey) + " needs exactly one catalog row");
    }
    expect(kInventory.size() == 84 && inventoryKeys.size() == 84 && mapped.size() == 84,
        "the inventory table must name 84 distinct booleans and 84 distinct rows");
    for (const SettingDescriptor& row : catalog)
        expect(mapped.contains(row.key) || kAdditions.contains(row.key), Q(row.key) + " is neither an inventory boolean nor a known addition");
    expect(catalog.size() == 84 + kAdditions.size(), "the catalog must hold the 84 inventory rows plus the 9 additions");

    for (const auto& key : revia::core::PreferenceStore::Writable())
    {
        if (key.type != revia::core::PreferenceType::Boolean)
            continue;
        const SettingDescriptor* row = FindSetting(key.name);
        expect(row != nullptr && row->store == SettingStore::Preference,
            "preference " + Q(key.name) + " must be a Preference row in the catalog");
    }
    for (const auto& key : revia::core::kOwnerKeys)
    {
        if (key.type != revia::core::OwnerKeyType::Bool)
            continue;
        const SettingDescriptor* row = FindSetting(std::string(key.key));
        expect(row != nullptr && row->store == SettingStore::SettingsFile && row->allCompanions,
            "owner setting " + Q(std::string(key.key)) + " must be an All companions SettingsFile row");
        if (row != nullptr)
            expect((row->apply == SettingApply::Live) == key.live, Q(row->key) + " must apply live exactly when the overlay allowlist says so");
    }

    // Startup preferences the session commits at once because something reads them while Revia runs
    // (design spec 5.7.5): their running value always follows the saved one, so a restart badge
    // would promise a restart the banner never asks for.
    for (const char* key : {"performance.enabled", "performance.interruptSongToSpeak"})
    {
        const SettingDescriptor* row = FindSetting(key);
        expect(row != nullptr && row->apply == SettingApply::Partial, Q(key) + " applies partly now and must be a Partial row");
    }
    // The microphone stops at once when it is turned off; turning it on needs the recognizer Start
    // opens, so only that direction waits for a restart.
    const SettingDescriptor* microphone = FindSetting("speechRecognition.enabled");
    expect(microphone != nullptr && microphone->apply == SettingApply::LiveWhenOffRestartWhenOn,
        "turning the microphone off applies at once and turning it on after a restart");
    // Screen capture is authority, like the camera: both screen switches ask before turning on.
    for (const char* key : {"vision.enabled", "vision.continuousAwareness"})
    {
        const SettingDescriptor* row = FindSetting(key);
        expect(row != nullptr && row->confirm == ConfirmWhen::TurningOn, Q(key) + " captures the screen and must ask before turning on");
    }

    // Tool-server templates expand per server, with sanitised objectNames.
    const SettingDescriptor* confirmation = FindSetting("mcp.servers[*].requireConfirmation");
    expect(confirmation != nullptr && IsServerTemplate(*confirmation), "the tool-server confirmation row must be a template");
    if (confirmation != nullptr)
    {
        const SettingDescriptor server = ServerRow(*confirmation, "web-search.v2");
        expect(server.key == "mcp.servers[web-search.v2].requireConfirmation" &&
                   server.objectName == "sw_mcp_web_search_v2_requireConfirmation" &&
                   server.requiresAll == std::vector<std::string>{"mcp.servers[web-search.v2].enabled"} && !IsServerTemplate(server),
            "a server row must carry the server id in its key and a sanitised id in its objectName");
    }

    // The CapabilityPanel switches the catalog points at exist under the names slice 0 froze.
    std::set<std::string> hostedNames = {"processEnabled", "processTaskExecution", "processCommandInterpreters", "browserEnabled",
        "browserNavigate", "browserInteract", "browserTaskInteraction", "browserLoopback"};
    for (const auto& name : names::kUnstyled)
        hostedNames.insert(std::string(name.name));
    for (const SettingDescriptor& row : catalog)
    {
        if (row.host == SettingHost::CapabilityPanel)
            expect(hostedNames.contains(row.objectName), Q(row.objectName) + " is not a known CapabilityPanel switch name");
    }
    std::set<QString> sectionIds;
    for (const SettingsSection section : SettingsSections())
        sectionIds.insert(QString::fromUtf8(SectionId(section)));
    expect(SettingsSections().size() == 11 && sectionIds.size() == 11, "Settings has 11 sections with distinct ids");
}

void CheckDialog(Expect& expect, revia::runtime::ReviaSession& session)
{
    FakeBackend fake;
    fake.Set("initiative.enabled", false);
    fake.Set("localApi.enabled", true);
    fake.Set("speechRecognition.handsFree", true);
    fake.choices["profile.displayName"] = QStringLiteral("Assistant");
    fake.status["llm.cloud.enabled"] = QStringLiteral("Provider example.com · API key missing: set EXAMPLE_KEY");
    fake.status["speechRecognition.requireWakeWord"] = QStringLiteral("Answers only when it hears: revia, rivia.");

    std::vector<std::pair<std::string, bool>> changed;
    int restarts = 0;
    int themeChoice = -1;
    int textSize = -1;
    bool trayAvailable = true;
    std::string openedPanel;
    int hides = 0;
    SettingsHooks hooks;
    hooks.valueChanged = [&](const std::string& key, const bool value) { changed.emplace_back(key, value); };
    hooks.themePreference = []() { return 2; };
    hooks.setThemePreference = [&](const int value) { themeChoice = value; };
    hooks.chatTextSize = []() { return 17; };
    hooks.setChatTextSize = [&](const int value) { textSize = value; };
    hooks.openPanel = [&](const char* key) { openedPanel = key; };
    hooks.restartApp = [&]() { ++restarts; };
    hooks.onHide = [&]() { ++hides; };
    hooks.companionName = []() { return QStringLiteral("Revia"); };
    hooks.trayAvailable = [&]() { return trayAvailable; };
    int iconRequests = 0;
    hooks.icon = [&](const char*)
    {
        ++iconRequests;
        QPixmap pixmap(16, 16);
        pixmap.fill(Qt::darkCyan);
        return QIcon(pixmap);
    };

    SettingsDialog dialog(fake.Make(), hooks, nullptr);
    dialog.resize(920, 640);
    dialog.show();
    Settle();
    auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsStack"));
    auto* search = dialog.findChild<QLineEdit*>(QStringLiteral("settingsSearch"));
    auto* sectionList = dialog.findChild<QListWidget*>(QStringLiteral("settingsSections"));
    expect(stack && search && sectionList, "the dialog needs its stack, search box and section list");
    if (!stack || !search || !sectionList)
        return;
    expect(dialog.windowTitle() == "Settings" && dialog.minimumSize() == QSize(760, 520), "Settings is titled and at least 760x520");
    expect(stack->property(names::kNavigationStackProperty).toBool(), "the section stack is marked for SelectNavigationPage");
    expect(dialog.SectionPages().size() == 11 && sectionList->count() == 11, "every section has a page and a list row");
    for (QWidget* page : dialog.SectionPages())
        expect(page->objectName().startsWith("settingsSection_") && page->property(names::kRoleProperty).toString() == "settingsSection",
            "section page " + page->objectName() + " carries its name and role");
    expect(search->hasFocus(), "the search box takes focus when Settings opens");
    expect(!sectionList->item(0)->icon().isNull() && dialog.findChild<QPushButton*>("themeDark") &&
               !dialog.findChild<QPushButton*>("themeDark")->icon().isNull(),
        "sections and theme choices take their icons from hooks.icon");
    {
        const int before = iconRequests;
        QPalette palette = dialog.palette();
        palette.setColor(QPalette::Window, Qt::black);
        dialog.setPalette(palette);
        Settle();
        expect(iconRequests > before, "a palette change fetches the icons again");
        dialog.setPalette(QPalette());
    }
    auto* banner = dialog.findChild<QFrame*>(QStringLiteral("settingsRestartBanner"));
    expect(dialog.PendingRestartCount() == 0 && banner && banner->isHidden(), "no restart is pending at first");

    // Every dialog row: a switch with a title, a description and its restart badge.
    for (const SettingDescriptor& descriptor : SettingCatalog())
    {
        if (descriptor.host != SettingHost::Dialog || IsServerTemplate(descriptor))
            continue;
        const QString name = Q(descriptor.objectName);
        auto* toggle = dialog.findChild<QCheckBox*>(name);
        auto* row = dialog.findChild<QFrame*>(name + "Row");
        expect(toggle != nullptr && dialog.Switch(descriptor.key) == toggle && dialog.Switch(descriptor.objectName) == toggle,
            name + " must be a switch the dialog returns by key and objectName");
        expect(row != nullptr && row->property(names::kClassProperty).toString() == "settingRow", name + "Row must be a setting row");
        if (toggle == nullptr || row == nullptr)
            continue;
        expect(toggle->property(names::kRoleProperty).toString() == "settingSwitch", name + " carries the settingSwitch role");
        const QLabel* title = row->findChild<QLabel*>(QStringLiteral("settingTitle"));
        const QLabel* description = row->findChild<QLabel*>(QStringLiteral("settingDescription"));
        expect(title && !title->text().isEmpty() && description && !description->text().isEmpty(), name + " needs a title and a description");
        expect(title && !title->text().contains('{') && description && !description->text().contains('{'), name + " text must be fully substituted");
        expect(!toggle->accessibleName().isEmpty() && !toggle->accessibleDescription().isEmpty(), name + " needs accessible text");
        const bool restart = descriptor.apply == SettingApply::Restart || descriptor.apply == SettingApply::LiveWhenOffRestartWhenOn;
        expect((Badge(row, "restart") != nullptr) == restart, name + " shows Applies after restart exactly when it waits for a restart");
        const QLabel* partial = Badge(row, "partial");
        expect((partial != nullptr) == (descriptor.apply == SettingApply::Partial) && (partial == nullptr || partial->text() == "Partly applies now"),
            name + " shows Partly applies now exactly when part of it applies at once");
        expect((Badge(row, "danger") != nullptr) == (descriptor.risk == SettingRisk::Dangerous), name + " shows Risky exactly when risky");
        expect((Badge(row, "global") != nullptr) == descriptor.allCompanions, name + " shows All companions exactly when global");
    }
    for (const std::string id : {"files", "web_search"})
    {
        for (const std::string suffix : {"_enabled", "_requireConfirmation"})
            expect(dialog.findChild<QCheckBox*>(Q("sw_mcp_" + id + suffix)) != nullptr, Q("sw_mcp_" + id + suffix) + " must exist per server");
    }
    expect(dialog.Switch("mcp.servers[web-search].enabled") == dialog.findChild<QCheckBox*>("sw_mcp_web_search_enabled"),
        "a server row is found by its instantiated key");
    expect(dialog.Descriptors().size() == 2 * fake.servers.size() + std::count_if(SettingCatalog().begin(), SettingCatalog().end(),
        [](const SettingDescriptor& row) { return row.host == SettingHost::Dialog && !IsServerTemplate(row); }),
        "Descriptors() lists every built switch row, servers included");

    for (const auto& name : names::kStyled)
    {
        static const std::set<std::string_view> builtHere = {"settingsDialog", "settingsNav", "settingsSearch", "settingsSections",
            "settingsStack", "settingsSectionTitle", "settingsSectionIntro", "settingsGroupTitle", "settingsGroup", "settingsDivider",
            "settingsRiskyHeader", "settingTitle", "settingDescription", "settingBadge", "settingWarning", "settingStatus",
            "settingsRestartBanner", "settingsRestartNow", "settingsRestartLater", "settingsNoResults", "themeSegment", "themeSystem",
            "themeLight", "themeDark", "chatTextSizeCombo", "profileTemperatureSpin", "profileMaxTokensSpin"};
        if (!builtHere.contains(name.name))
            continue;
        const QString objectName = QString::fromUtf8(name.name.data(), static_cast<int>(name.name.size()));
        expect(dialog.objectName() == objectName || dialog.findChild<QWidget*>(objectName) != nullptr, objectName + " must exist in the dialog");
    }
    for (const char* rowName : {"themeSegmentRow", "chatTextSizeComboRow"})
        expect(dialog.findChild<QFrame*>(rowName) != nullptr, QString(rowName) + " must exist");

    // Appearance rows talk to the hooks.
    auto* dark = dialog.findChild<QPushButton*>(QStringLiteral("themeDark"));
    auto* light = dialog.findChild<QPushButton*>(QStringLiteral("themeLight"));
    expect(dark && dark->isChecked(), "the theme segment shows the stored preference");
    if (light)
        light->click();
    expect(themeChoice == 1, "choosing Light sets the theme preference");
    auto* sizeCombo = dialog.findChild<QComboBox*>(QStringLiteral("chatTextSizeCombo"));
    expect(sizeCombo && sizeCombo->currentData().toInt() == 17, "the text size combo shows the stored size");
    if (auto* open = dialog.findChild<QPushButton*>(QStringLiteral("settingsOpenPanel_voice")))
        open->click();
    expect(openedPanel == "voice", "Open Voice window opens the Voice panel");

    // A risky switch asks first; Cancel is the default and writes nothing.
    auto* trusted = dialog.findChild<QCheckBox*>(QStringLiteral("sw_localApi_trustedPrivate"));
    expect(trusted && trusted->isEnabled() && !trusted->isChecked(), "Trust the local API starts off and reachable");
    if (trusted)
    {
        const std::size_t writes = fake.writes.size();
        trusted->click();
        QMessageBox* box = OpenConfirmation(&dialog);
        expect(box != nullptr, "turning on a risky row opens settingConfirmDialog");
        if (box)
        {
            expect(box->windowTitle() == "Turn on Trust the local API as me?", "the confirmation names the switch: " + box->windowTitle());
            expect(box->windowTitle() != "Confirm Revia action" && box->windowTitle() != "Approve this action", "reserved titles stay unused");
            expect(box->defaultButton() == box->button(QMessageBox::Cancel), "Cancel is the default button");
            expect(box->text() == box->windowTitle(), "the question is the message itself, not only the caption");
            expect(box->informativeText().contains("speaks as you") && box->informativeText().contains("change this back"),
                "the warning explains the risk");
            expect(ButtonWithText(box, "Turn on") && ButtonWithText(box, "Turn on")->property("destructive").toBool(),
                "Turn on is drawn as the risky choice");
            expect(!trusted->isChecked() && fake.writes.size() == writes, "nothing changes while the question is open");
            box->button(QMessageBox::Cancel)->click();
            Settle();
        }
        expect(!trusted->isChecked() && fake.writes.size() == writes, "Cancel leaves the switch off and writes nothing");
        trusted->click();
        box = OpenConfirmation(&dialog);
        if (QAbstractButton* accept = box ? ButtonWithText(box, "Turn on") : nullptr)
            accept->click();
        Settle();
        expect(trusted->isChecked() && !fake.writes.empty() && fake.writes.back() == std::pair<std::string, bool>{"localApi.trustedPrivate", true},
            "Turn on writes the change");
        const QLabel* warning = RowLabel(&dialog, "sw_localApi_trustedPrivateRow", "settingWarning");
        expect(warning && !warning->isHidden() && warning->text().contains("speaks as you"), "a risky switch that is on shows its warning");
    }
    // Confirm-off: turning off the wake word asks.
    if (auto* wakeWord = dialog.findChild<QCheckBox*>(QStringLiteral("sw_speechRecognition_requireWakeWord")))
    {
        expect(wakeWord->isChecked() && wakeWord->isEnabled(), "Require the wake word starts on with hands-free listening on");
        const QLabel* description = RowLabel(&dialog, "sw_speechRecognition_requireWakeWordRow", "settingDescription");
        expect(description && description->text().contains("revia, rivia"), "the wake word row lists the configured wake words");
        wakeWord->click();
        QMessageBox* box = OpenConfirmation(&dialog);
        expect(box && box->windowTitle() == "Turn off Require the wake word?", "turning off a confirm-off row asks");
        if (QAbstractButton* accept = box ? ButtonWithText(box, "Turn off") : nullptr)
            accept->click();
        Settle();
        expect(!wakeWord->isChecked() && fake.writes.back() == std::pair<std::string, bool>{"speechRecognition.requireWakeWord", false},
            "Turn off writes the change");
    }

    // A write in flight keeps the switch busy until done runs.
    if (auto* review = dialog.findChild<QCheckBox*>(QStringLiteral("aiFilterCheck")))
    {
        fake.deferNext = true;
        review->click();
        Settle();
        const QLabel* status = RowLabel(&dialog, "aiFilterCheckRow", "settingStatus");
        expect(!review->isEnabled() && status && !status->isHidden() && status->text().startsWith("Saving"),
            "a pending write disables the switch and says Saving");
        if (fake.deferred)
            fake.deferred({true, QStringLiteral("AI review is on.")});
        Settle();
        expect(review->isEnabled() && review->isChecked() && status && status->text() == "AI review is on." &&
                   status->property("tone").toString() == "success",
            "the switch comes back with the backend's message when done runs");
        expect(!changed.empty() && changed.back() == std::pair<std::string, bool>{"responseFilter.aiReviewEnabled", true},
            "valueChanged runs after a successful write");
    }
    // A write whose answer never comes (the window drops answers for a retired session) does not keep
    // its row busy for good: AbandonPendingWrites releases it, and an answer that still arrives
    // afterwards neither finishes a newer write nor reports a change.
    if (auto* review = dialog.findChild<QCheckBox*>(QStringLiteral("aiFilterCheck")))
    {
        const QLabel* status = RowLabel(&dialog, "aiFilterCheckRow", "settingStatus");
        fake.deferNext = true;
        review->click();
        Settle();
        SettingDone lost = std::move(fake.deferred);
        fake.deferred = nullptr;
        dialog.AbandonPendingWrites();
        expect(lost && review->isEnabled() && review->isChecked() && status && !status->text().startsWith("Saving"),
            "AbandonPendingWrites enables the switch again, shows the value last read and drops Saving");
        dialog.Refresh();
        expect(!review->isChecked(), "the next Refresh shows what was saved");
        const std::size_t notified = changed.size();
        fake.deferNext = true;
        review->click();
        Settle();
        dialog.Refresh();
        expect(!review->isEnabled(), "a write in flight stays pending through Refresh");
        if (lost)
            lost({false, QStringLiteral("Late answer.")});
        Settle();
        expect(!review->isEnabled() && review->isChecked() && status && status->text().startsWith("Saving") && changed.size() == notified,
            "the abandoned write's late answer leaves the newer write pending");
        if (fake.deferred)
            fake.deferred({true, QStringLiteral("AI review is on.")});
        fake.deferred = nullptr;
        Settle();
        expect(review->isEnabled() && review->isChecked() && status && status->text() == "AI review is on." && changed.size() == notified + 1,
            "the newer write finishes with its own answer");
    }
    // A refused write puts the switch back and shows the error.
    if (auto* perception = dialog.findChild<QCheckBox*>(QStringLiteral("sw_perception_enabled")))
    {
        fake.refused.insert("perception.enabled");
        const bool before = perception->isChecked();
        perception->click();
        Settle();
        const QLabel* status = RowLabel(&dialog, "sw_perception_enabledRow", "settingStatus");
        expect(perception->isChecked() == before && status && status->property("tone").toString() == "error" &&
                   status->text().contains("Refused"),
            "a refused write reverts the switch and shows why");
    }

    // The restart banner counts rows whose saved value differs from the running one.
    auto* restartNow = dialog.findChild<QPushButton*>(QStringLiteral("settingsRestartNow"));
    auto* later = dialog.findChild<QPushButton*>(QStringLiteral("settingsRestartLater"));
    // Trust the local API (restart-only) was turned on above and is still pending.
    const int base = dialog.PendingRestartCount();
    expect(base == 1 && banner && !banner->isHidden(), "the earlier restart-only change is pending");
    const auto bannerText = [&](const int count)
        { return QStringLiteral("Restart Revia to apply %1 change%2.").arg(count).arg(count == 1 ? "" : "s"); };
    auto* embedding = dialog.findChild<QCheckBox*>(QStringLiteral("sw_embedding_enabled"));
    auto* autoTune = dialog.findChild<QCheckBox*>(QStringLiteral("sw_llm_autoTune"));
    auto* restoreOpen = dialog.findChild<QCheckBox*>(QStringLiteral("sw_panels_restoreOpen"));
    if (embedding && autoTune && restoreOpen && banner && restartNow && later)
    {
        embedding->click();
        Settle();
        const QLabel* badge = Badge(dialog.findChild<QFrame*>("sw_embedding_enabledRow"), "restart");
        expect(dialog.PendingRestartCount() == base + 1 && !banner->isHidden() &&
                   banner->findChild<QLabel*>()->text() == bannerText(base + 1),
            "a restart-only change raises the banner's count");
        expect(badge && badge->text() == "Restart to apply", "the changed row's badge says it waits for a restart");
        expect(!restartNow->isHidden(), "Restart now shows when the window can restart");
        restartNow->click();
        expect(restarts == 1, "Restart now asks the window to restart");
        later->click();
        expect(banner->isHidden(), "Later hides the banner");
        autoTune->click();
        Settle();
        expect(dialog.PendingRestartCount() == base + 2 && !banner->isHidden(), "a further change brings the banner back");
        restoreOpen->click();
        Settle();
        expect(dialog.PendingRestartCount() == base + 3 && QSettings().value("panels/restoreOpen").toBool(),
            "a restart-only Shell row counts too");
        for (QCheckBox* toggle : {embedding, autoTune, restoreOpen})
            toggle->click();
        Settle();
        expect(dialog.PendingRestartCount() == base && banner->findChild<QLabel*>()->text() == bannerText(base),
            "changing them back takes them out of the count");
        expect(badge && badge->text() == "Applies after restart", "the badge returns to its resting text");
        if (trusted)
            trusted->click(); // turning a risky switch off needs no confirmation
        Settle();
        expect(dialog.PendingRestartCount() == 0 && banner->isHidden() && trusted && !trusted->isChecked(),
            "with nothing pending the banner goes away");
    }
    // Part of a Partial row applies at once and the rest at the next start, so a change is counted
    // until then and the badge says so; changing it back puts the resting text back.
    if (auto* interrupt = dialog.findChild<QCheckBox*>(QStringLiteral("sw_performance_interruptSongToSpeak")); interrupt && banner)
    {
        const int pending = dialog.PendingRestartCount();
        interrupt->click();
        Settle();
        const QLabel* badge = Badge(dialog.findChild<QFrame*>("sw_performance_interruptSongToSpeakRow"), "partial");
        expect(!interrupt->isChecked() && dialog.PendingRestartCount() == pending + 1 && badge && badge->text() == "Restart to apply",
            "changing a Partial row counts it for a restart and says so");
        interrupt->click();
        Settle();
        expect(dialog.PendingRestartCount() == pending && badge && badge->text() == "Partly applies now",
            "changing a Partial row back takes it out of the count");
    }

    // Dependencies use the saved values.
    auto* initiative = dialog.findChild<QCheckBox*>(QStringLiteral("initiativeCheck"));
    auto* curiosity = dialog.findChild<QCheckBox*>(QStringLiteral("curiosityCheck"));
    auto* spinner = new QSpinBox();
    dialog.AddCustomRow(SettingsSection::Chat, "Starting conversations", "At most N unprompted openings per hour (0 = never)",
        "How often {name} may start a conversation.", spinner, "initiativeMaxSpinRow", {"initiative.enabled"});
    auto* customRow = dialog.findChild<QFrame*>(QStringLiteral("initiativeMaxSpinRow"));
    expect(customRow && customRow->property(names::kClassProperty).toString() == "settingRow", "a custom row is a setting row");
    if (initiative && curiosity)
    {
        expect(!curiosity->isEnabled() && curiosity->toolTip() == "Turn on Let Revia start conversations first",
            "a row whose requirement is off is disabled with the reason");
        expect(!spinner->isEnabled(), "a custom row follows its requiresAll");
        initiative->click();
        Settle();
        expect(curiosity->isEnabled() && spinner->isEnabled(), "turning the requirement on enables its dependents");
        dialog.SetValue("initiative.enabled", false);
        expect(!curiosity->isEnabled() && !spinner->isEnabled() && !initiative->isChecked(), "SetValue moves dependents without writing");
        dialog.SetValue("initiative.enabled", true);
    }
    if (auto* autoSend = dialog.findChild<QCheckBox*>(QStringLiteral("autoSendVoiceCheck")))
    {
        expect(!autoSend->isEnabled() && autoSend->toolTip().contains("Hands-free listening"), "auto-send is disabled while hands-free is on");
        dialog.SetValue("speechRecognition.handsFree", false);
        expect(autoSend->isEnabled(), "auto-send comes back when hands-free is off");
        dialog.SetValue("speechRecognition.handsFree", true);
        expect(!autoSend->isEnabled(), "SetValue(handsFree, true) disables auto-send");
    }
    int linkClicks = 0;
    dialog.AddLinkRow(SettingsSection::Voice, "Listening", "Voice generation device", "Pick the device in the Resources window.",
        "Open Resources", [&]() { ++linkClicks; }, "voiceDeviceLinkRow");
    if (auto* linkRow = dialog.findChild<QFrame*>(QStringLiteral("voiceDeviceLinkRow")))
    {
        if (auto* button = linkRow->findChild<QPushButton*>())
            button->click();
    }
    expect(linkClicks == 1, "a link row's button runs its action");

    // An inverted row shows the opposite of the stored value and writes the opposite.
    if (auto* allowInternet = dialog.findChild<QCheckBox*>(QStringLiteral("sw_image_allowInternet")))
    {
        expect(!allowInternet->isChecked(), "image.offline=true shows as picture generation not using the internet");
        allowInternet->click();
        if (QMessageBox* box = OpenConfirmation(&dialog))
        {
            if (QAbstractButton* accept = ButtonWithText(box, "Turn on"))
                accept->click();
        }
        Settle();
        expect(allowInternet->isChecked() && fake.writes.back() == std::pair<std::string, bool>{"image.offline", false},
            "turning the inverted switch on writes image.offline=false");
    }

    // Shell rows write QSettings, and closing-to-tray depends on a tray.
    if (auto* tray = dialog.findChild<QCheckBox*>(QStringLiteral("sw_window_closeToTray")))
    {
        expect(tray->isChecked(), "closing to the tray is on by default");
        tray->click();
        Settle();
        const QLabel* description = RowLabel(&dialog, "sw_window_closeToTrayRow", "settingDescription");
        expect(!QSettings().value("window/closeToTray").toBool() && description && description->text() == "Closing the window quits Revia.",
            "turning it off is saved and the description says what closing does now");
        expect(changed.back() == std::pair<std::string, bool>{"window/closeToTray", false}, "Shell writes call valueChanged too");
        trayAvailable = false;
        dialog.Refresh();
        expect(!tray->isEnabled() && tray->toolTip() == "No system tray is available", "without a tray the row is disabled");
        trayAvailable = true;
        dialog.Refresh();
    }

    // The whole row is the switch's hit area: a click on its description turns a switch, and a
    // risky row asks first, exactly as its switch does.
    dialog.ShowSection(SettingsSection::General);
    Settle();
    if (auto* follow = dialog.findChild<QCheckBox*>(QStringLiteral("sw_activity_follow")))
    {
        QLabel* description = RowLabel(&dialog, "sw_activity_followRow", "settingDescription");
        const bool before = follow->isChecked();
        ClickOn(description);
        expect(description && follow->isChecked() != before && QSettings().value("activity/follow").toBool() != before,
            "clicking a row's description turns its switch and saves it");
        ClickOn(description);
        expect(follow->isChecked() == before && QSettings().value("activity/follow").toBool() == before, "a second click turns it back");
    }
    if (trusted)
    {
        dialog.ShowSection(SettingsSection::Integrations);
        Settle();
        const std::size_t writes = fake.writes.size();
        ClickOn(RowLabel(&dialog, "sw_localApi_trustedPrivateRow", "settingDescription"));
        QMessageBox* box = OpenConfirmation(&dialog);
        expect(box != nullptr && !trusted->isChecked(), "clicking a risky row asks before turning it on");
        if (box)
            box->button(QMessageBox::Cancel)->click();
        Settle();
        expect(!trusted->isChecked() && fake.writes.size() == writes, "and Cancel from a row click writes nothing");
    }

    // Persistent statuses and backend status text.
    dialog.SetRowStatus("speech.enabled", "Voice is unavailable: fixture", "warning");
    const QLabel* speechStatus = RowLabel(&dialog, "speechCheckRow", "settingStatus");
    expect(speechStatus && !speechStatus->isHidden() && speechStatus->text() == "Voice is unavailable: fixture" &&
               speechStatus->property("tone").toString() == "warning",
        "SetRowStatus shows a persistent status");
    const QLabel* cloudStatus = RowLabel(&dialog, "sw_llm_cloud_enabledRow", "settingStatus");
    expect(cloudStatus && cloudStatus->text().contains("API key missing") && cloudStatus->property("tone").toString() == "warning",
        "the cloud row shows the backend's status line, as a warning when the key is missing");
    const QLabel* profileGroup = nullptr;
    for (QLabel* label : dialog.findChildren<QLabel*>(QStringLiteral("settingsGroupTitle")))
    {
        if (label->text().startsWith("This profile"))
            profileGroup = label;
    }
    expect(profileGroup && profileGroup->text() == "This profile (Assistant)", "the profile group names the active profile");
    // The value beside a profile override switch is saved only when it changed: a spin box reports
    // editingFinished on every focus change, and each write saves the whole profile.
    auto* temperature = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("profileTemperatureSpin"));
    expect(temperature && qFuzzyCompare(temperature->value(), 0.75) && !temperature->isEnabled(),
        "the temperature spin shows the profile's value and waits for its switch");
    fake.Set("profile.hasTemperatureOverride", true);
    dialog.Refresh();
    if (temperature)
    {
        expect(temperature->isEnabled(), "the temperature spin follows its switch");
        const QLabel* status = RowLabel(&dialog, "sw_profile_temperatureOverrideRow", "settingStatus");
        dialog.ShowSection(SettingsSection::Chat);
        // The confirmations above took activation with them when they closed (offscreen).
        dialog.activateWindow();
        Settle();
        temperature->setFocus(Qt::TabFocusReason);
        const bool focused = temperature->hasFocus();
        search->setFocus(Qt::TabFocusReason);
        Settle();
        expect(focused && !temperature->hasFocus() && fake.profileWrites.empty(), "passing through the spin without a change writes nothing");
        temperature->setValue(1.1);
        emit temperature->editingFinished();
        expect(fake.profileWrites.size() == 1 && fake.profileWrites.back().first == "temperature" &&
                   qFuzzyCompare(fake.profileWrites.back().second, 1.1),
            "a changed value writes the profile");
        emit temperature->editingFinished();
        expect(fake.profileWrites.size() == 1, "leaving the spin again with the saved value writes nothing more");
        fake.refuseProfileWrites = true;
        temperature->setValue(1.3);
        emit temperature->editingFinished();
        expect(fake.profileWrites.size() == 2 && qFuzzyCompare(temperature->value(), 1.1) && status &&
                   status->text() == "Save or discard your changes in Profiles first." && status->property("tone").toString() == "error",
            "a refused value goes back to the saved one and says why");
        fake.refuseProfileWrites = false;
        emit temperature->editingFinished();
        expect(fake.profileWrites.size() == 2, "the value put back after a refusal is not written again");
        // Esc while a value is being typed puts the saved one back and writes nothing, rather than
        // closing Settings, which would save the typed value as the spin lost focus.
        auto* temperatureSwitch = dialog.findChild<QCheckBox*>(QStringLiteral("sw_profile_temperatureOverride"));
        temperature->setFocus(Qt::TabFocusReason);
        temperature->setValue(1.5);
        const bool typing = temperature->hasFocus();
        PressKey(temperature, Qt::Key_Escape);
        expect(typing && dialog.isVisible() && qFuzzyCompare(temperature->value(), 1.1) && fake.profileWrites.size() == 2,
            "Esc in a profile spin restores the saved value, writes nothing and leaves Settings open");
        expect(temperatureSwitch && temperatureSwitch->hasFocus(), "the focus moves to the spin's switch, so a second Esc can close Settings");
        // An abandoned value write drops its Saving line, and its late answer is ignored.
        fake.deferNextProfileWrite = true;
        temperature->setValue(1.2);
        emit temperature->editingFinished();
        const bool saving = status && status->text().startsWith("Saving");
        dialog.AbandonPendingWrites();
        expect(saving && status && !status->text().startsWith("Saving"), "AbandonPendingWrites clears a profile value's Saving line");
        if (fake.deferredProfileWrite)
            fake.deferredProfileWrite({false, QStringLiteral("Late refusal.")});
        fake.deferredProfileWrite = nullptr;
        expect(qFuzzyCompare(temperature->value(), 1.2) && status && status->text() != "Late refusal.",
            "the late answer to an abandoned value write changes nothing");
    }

    // "Reset to default" never makes the change a switch would have asked about. The overlay's
    // default for improvement.verify is whatever Config/settings.json ships, which is on.
    fake.Set("improvement.enabled", true);
    fake.Set("improvement.verify", false);
    fake.resetTo["improvement.verify"] = true;
    dialog.Refresh();
    dialog.ShowSection(SettingsSection::Advanced);
    Settle();
    auto* verify = dialog.findChild<QCheckBox*>(QStringLiteral("sw_improvement_verify"));
    auto* verifyRow = dialog.findChild<QFrame*>(QStringLiteral("sw_improvement_verifyRow"));
    expect(verify && verifyRow && verify->isEnabled() && !verify->isChecked(), "Build and test suggested improvements starts off and reachable");
    if (verify && verifyRow)
    {
        expect(!ResetOffered(verifyRow, true) && fake.resets.empty() && !verify->isChecked() && !fake.Entry("improvement.verify").value,
            "a risky switch that is off offers no reset that could turn it on unasked");
        fake.Set("improvement.verify", true);
        fake.resetTo["improvement.verify"] = false;
        dialog.Refresh();
        expect(verify->isChecked() && ResetOffered(verifyRow, true) && fake.resets == std::vector<std::string>{"improvement.verify"} &&
                   !verify->isChecked(),
            "once it is on, a reset can only leave it on or take it off, so it is offered and runs");
    }
    if (auto* reviewRow = dialog.findChild<QFrame*>(QStringLiteral("aiFilterCheckRow")))
        expect(ResetOffered(reviewRow, false), "a switch that never asks offers its reset either way");

    // A renamed profile re-renders its group's title.
    fake.choices["profile.displayName"] = QStringLiteral("Evening");
    dialog.Refresh();
    expect(profileGroup && profileGroup->text() == "This profile (Evening)", "Refresh re-renders group titles");
    fake.choices["profile.displayName"] = QStringLiteral("Assistant");
    dialog.Refresh();

    // Companion name substitution.
    const QLabel* bargeIn = RowLabel(&dialog, "bargeInCheckRow", "settingTitle");
    const QLabel* voiceIntro = dialog.SectionPage(SettingsSection::Voice)->findChild<QLabel*>(QStringLiteral("settingsSectionIntro"));
    expect(bargeIn && bargeIn->text() == "Let me interrupt Revia", "{name} becomes the companion's name");
    expect(voiceIntro && voiceIntro->text().contains("Revia"), "{name} in a section intro becomes the companion's name");
    dialog.SetCompanionName("Nova");
    expect(bargeIn && bargeIn->text() == "Let me interrupt Nova", "SetCompanionName re-renders the rows");
    expect(voiceIntro && voiceIntro->text().contains("Nova") && !voiceIntro->text().contains("Revia"),
        "SetCompanionName re-renders the section intros");
    for (const QLabel* label : dialog.findChildren<QLabel*>())
        expect(!label->text().contains("{name}") && !label->text().contains("{profile}"), "no placeholder may show: " + label->text());

    // Search hides what does not match, counts hits and says when nothing matches.
    dialog.ShowSection(SettingsSection::General);
    search->setText("microphone");
    Settle();
    QListWidgetItem* voiceItem = sectionList->item(static_cast<int>(SettingsSection::Voice));
    QListWidgetItem* generalItem = sectionList->item(static_cast<int>(SettingsSection::General));
    auto* micRow = dialog.findChild<QFrame*>(QStringLiteral("sw_speechRecognition_enabledRow"));
    auto* speechRow = dialog.findChild<QFrame*>(QStringLiteral("speechCheckRow"));
    expect(micRow && !micRow->isHidden() && speechRow && speechRow->isHidden(), "search keeps matching rows and hides the rest");
    expect(voiceItem && !voiceItem->isHidden() && voiceItem->data(Qt::UserRole + 1).toString().toInt() >= 2,
        "the Voice section shows its hit count");
    expect(generalItem && generalItem->isHidden(), "sections without hits are hidden");
    expect(stack->currentWidget() == dialog.SectionPage(SettingsSection::Voice), "search moves to the first section with hits");
    QWidget* singing = GroupBox(dialog.SectionPage(SettingsSection::Voice), QStringLiteral("Singing"));
    QWidget* listening = GroupBox(dialog.SectionPage(SettingsSection::Voice), QStringLiteral("Listening"));
    expect(singing && singing->isHidden() && listening && !listening->isHidden(),
        "search hides a group without matching rows and keeps a group with them");
    // Down in the search box moves into the section list, and Ctrl+F comes back with the query
    // selected. The confirmation above took activation with it when it closed (offscreen).
    dialog.activateWindow();
    Settle();
    search->setFocus(Qt::OtherFocusReason);
    PressKey(search, Qt::Key_Down);
    expect(sectionList->hasFocus(), "Down in the search box moves into the section list");
    PressKey(sectionList, Qt::Key_F, Qt::ControlModifier);
    expect(search->hasFocus() && search->selectedText() == "microphone", "Ctrl+F focuses the search box and selects the query");
    auto* noResults = dialog.findChild<QLabel*>(QStringLiteral("settingsNoResults"));
    expect(noResults && noResults->isHidden(), "the no-results line stays hidden while something matches");
    search->setText("zzqx");
    Settle();
    expect(noResults && !noResults->isHidden() && noResults->text().contains("zzqx") && stack->isHidden(),
        "a query with no hits says so");
    expect(noResults && noResults->text().contains(QChar(0x201C)) && noResults->text().contains(QChar(0x201D)) &&
               !noResults->text().contains(QChar(0x00E2)),
        "the no-results line quotes the query with curly quotes, not mojibake");
    search->setFocus();
    PressKey(search, Qt::Key_Escape);
    expect(search->text().isEmpty() && dialog.isVisible() && micRow && !micRow->isHidden() && speechRow && !speechRow->isHidden() &&
               singing && !singing->isHidden(),
        "Esc clears the search first and restores every row and group");
    PressKey(search, Qt::Key_Escape);
    expect(!dialog.isVisible() && hides >= 1, "a second Esc closes Settings and runs onHide");
    dialog.show();
    Settle();

    // Section navigation and focusing a setting.
    expect(dialog.ShowSection(QStringLiteral("voice")) && stack->currentWidget() == dialog.SectionPage(SettingsSection::Voice),
        "ShowSection accepts a section id");
    expect(dialog.ShowSection(QStringLiteral("Advanced")) && stack->currentWidget() == dialog.SectionPage(SettingsSection::Advanced),
        "ShowSection accepts a section name");
    expect(!dialog.ShowSection(QStringLiteral("no such thing")), "ShowSection refuses an unknown name");
    dialog.FocusSetting("speech.enabled");
    Settle();
    expect(stack->currentWidget() == dialog.SectionPage(SettingsSection::Voice) && speechRow && speechRow->property("highlight").toBool(),
        "FocusSetting shows the section and highlights the row");

    dialog.SetLocked(true);
    expect(!dialog.isEnabled(), "SetLocked disables Settings");
    dialog.SetLocked(false);
    expect(dialog.isEnabled(), "SetLocked(false) enables it again");

    // Hosting the real panels the way the window will, reflow hook included: the capability switches
    // become reachable and searchable.
    auto* panel = new CapabilityPanel(session, {}, nullptr);
    dialog.SetHostedWidget(SettingsSection::Permissions, "permissions", panel, 0);
    QWidget* internetGroup = panel->DetachInternetGroup();
    expect(internetGroup && internetGroup->findChild<QCheckBox*>("internetCheck") && internetGroup->findChild<QCheckBox*>("browserEnabled"),
        "the detached internet group holds the internet and browser switches");
    expect(panel->findChild<QCheckBox*>("internetCheck") == nullptr && panel->DetachInternetGroup() == internetGroup,
        "after detaching, the panel no longer lays the group out and returns the same container");
    dialog.SetHostedWidget(SettingsSection::Internet, "internet", internetGroup, 0);
    // What the window's ApplySettingsLayout does: reflow the panel for the page's width less its margins.
    dialog.SetResizeHook([panel](const QSize size) { panel->ApplyContentWidth(size.width() - 80); });
    Settle();
    expect(dialog.Switch("capabilities.desktopControl.pointer") == panel->findChild<QCheckBox*>("pointerCheck") &&
               dialog.Switch("capabilities.internet.enabled") == internetGroup->findChild<QCheckBox*>("internetCheck"),
        "Switch finds hosted capability switches");
    const QLabel* learning = RowLabel(&dialog, "sw_initiative_autonomousLearningEnabledRow", "settingStatus");
    expect(learning && learning->text() == "Works best with Research without being asked on", "a soft requirement shows a hint");
    search->setText("mouse");
    Settle();
    QListWidgetItem* permissionsItem = sectionList->item(static_cast<int>(SettingsSection::Permissions));
    expect(permissionsItem && !permissionsItem->isHidden() && !permissionsItem->data(Qt::UserRole + 1).toString().isEmpty(),
        "search finds hosted capability switches");
    search->clear();
    dialog.FocusSetting("capabilities.desktopControl.pointer");
    Settle();
    auto* pointer = panel->findChild<QCheckBox*>("pointerCheck");
    expect(stack->currentWidget() == dialog.SectionPage(SettingsSection::Permissions) && pointer &&
               pointer->parentWidget()->property("highlight").toBool(),
        "FocusSetting reaches a hosted switch");

    // Esc in a list being typed in a hosted editor keeps Settings open, the text and the focus: closing
    // would throw the list away (the window reverts unsaved lists when Settings hides). Tab leaves
    // the editor, and Esc in an empty one closes Settings as usual.
    dialog.ShowSection(SettingsSection::Permissions);
    Settle();
    if (auto* executables = panel->findChild<QPlainTextEdit*>(QStringLiteral("processApprovedExecutables")))
    {
        const int hidesBefore = hides;
        executables->setFocus(Qt::TabFocusReason);
        executables->setPlainText(QStringLiteral("C:\\Tools\\fixture.exe"));
        PressKey(executables, Qt::Key_Escape);
        expect(dialog.isVisible() && hides == hidesBefore && executables->hasFocus() && executables->toPlainText() == "C:\\Tools\\fixture.exe",
            "Esc in a hosted list editor keeps Settings open with the typed list");
        PressKey(executables, Qt::Key_Tab);
        expect(!executables->hasFocus() && executables->toPlainText() == "C:\\Tools\\fixture.exe", "Tab leaves the list editor without typing a tab");
        panel->RevertUnsavedLists();
        executables->setFocus(Qt::TabFocusReason);
        PressKey(executables, Qt::Key_Escape);
        expect(executables->toPlainText().isEmpty() && !dialog.isVisible() && hides == hidesBefore + 1, "Esc in an empty list editor closes Settings");
        dialog.show();
        dialog.activateWindow();
        Settle();
    }
    else
    {
        expect(false, "the hosted panel needs its approved executables editor");
    }
    // A spin box in a custom row: the window saves it when it loses focus, so Esc leaves it alone.
    {
        dialog.ShowSection(SettingsSection::Chat);
        Settle();
        int finished = 0;
        const auto connection = QObject::connect(spinner, &QSpinBox::editingFinished, [&finished]() { ++finished; });
        spinner->setFocus(Qt::TabFocusReason);
        const bool spinnerFocused = spinner->hasFocus();
        PressKey(spinner, Qt::Key_Escape);
        expect(spinnerFocused && dialog.isVisible() && spinner->hasFocus() && finished == 0,
            "Esc in a custom row's spin box neither closes Settings nor finishes the edit");
        QObject::disconnect(connection);
        search->setFocus(Qt::OtherFocusReason);
    }

    // The wheel over a value control that has no focus scrolls the page instead of changing the value:
    // the profile spins save the profile, the text size applies at once, and one notch down from the
    // default task mode would apply "Free" without a question.
    QList<QWidget*> valueControls;
    for (QAbstractSpinBox* spin : dialog.findChildren<QAbstractSpinBox*>())
        valueControls.push_back(spin);
    for (QComboBox* combo : dialog.findChildren<QComboBox*>())
        valueControls.push_back(combo);
    for (const QWidget* control : valueControls)
    {
        const QString name = control->objectName().isEmpty() ? QString::fromUtf8(control->metaObject()->className()) : control->objectName();
        expect(control->focusPolicy() == Qt::StrongFocus, name + " must take focus only from a click or Tab");
    }
    if (temperature)
    {
        dialog.ShowSection(SettingsSection::Chat);
        search->setFocus(Qt::OtherFocusReason);
        const double before = temperature->value();
        const std::size_t profileWrites = fake.profileWrites.size();
        expect(!WheelTaken(temperature) && qFuzzyCompare(temperature->value(), before) && !temperature->hasFocus(),
            "the wheel leaves an unfocused profile spin alone");
        search->setFocus(Qt::OtherFocusReason);
        Settle();
        expect(fake.profileWrites.size() == profileWrites, "and nothing is written when focus moves on");
    }
    if (sizeCombo)
    {
        dialog.ShowSection(SettingsSection::Appearance);
        const int before = sizeCombo->currentIndex();
        expect(!WheelTaken(sizeCombo) && sizeCombo->currentIndex() == before && textSize == -1,
            "the wheel leaves an unfocused text size alone and applies nothing");
    }
    if (auto* mode = panel->findChild<QComboBox*>(QStringLiteral("taskApprovalMode")))
    {
        dialog.ShowSection(SettingsSection::Permissions);
        search->setFocus(Qt::OtherFocusReason);
        const int before = mode->currentIndex();
        expect(!WheelTaken(mode) && mode->currentIndex() == before && mode->focusPolicy() == Qt::StrongFocus,
            "the wheel leaves the unfocused task approval mode alone");
    }
    // Once the owner has clicked or tabbed into a value control, the wheel changes it as usual.
    dialog.ShowSection(SettingsSection::Chat);
    Settle();
    spinner->setValue(5);
    spinner->setFocus(Qt::TabFocusReason);
    const int spinnerBefore = spinner->value();
    expect(spinner->hasFocus() && WheelTaken(spinner) && spinner->value() != spinnerBefore, "the wheel steps a focused spin box");
    spinner->setValue(spinnerBefore);
    search->setFocus(Qt::OtherFocusReason);

    // Every section fits 760x540 without horizontal scrolling, the hosted panel included.
    dialog.resize(760, 540);
    Settle();
    expect(dialog.findChild<QWidget*>(QStringLiteral("settingsNav"))->width() == 168, "the section list narrows below 900px");
    for (const SettingsSection section : SettingsSections())
    {
        dialog.ShowSection(section);
        Settle();
        auto* page = dynamic_cast<QScrollArea*>(dialog.SectionPage(section));
        expect(page && page->horizontalScrollBar()->maximum() == 0, page->objectName() + " must fit 760x540 without horizontal scrolling");
    }

    // Hiding Settings saves its geometry, and the next dialog opens with it. The size stays inside
    // the offscreen screen, which a restored geometry is fitted to.
    dialog.resize(780, 540);
    Settle();
    dialog.hide();
    expect(QSettings().value("settingsDialog/geometry").toByteArray() == dialog.saveGeometry(), "hiding Settings saves its geometry");
    {
        FakeBackend other;
        SettingsDialog restored(other.Make(), {}, nullptr);
        expect(restored.size() == QSize(780, 540), "a new Settings dialog opens with the saved size");
    }
    QSettings().remove("settingsDialog/geometry");
}

// Enter presses nothing that lacks focus. A shown QDialog makes its first auto-default button the
// default and presses it on Enter from anywhere; with the capability panel hosted that button was
// Stop desktop control. A fresh dialog, shown once with the panel in place, the way the window
// builds it.
void CheckEnterKey(Expect& expect, revia::runtime::ReviaSession& session)
{
    FakeBackend fake;
    SettingsDialog dialog(fake.Make(), {}, nullptr);
    auto* panel = new CapabilityPanel(session, {}, nullptr);
    dialog.SetHostedWidget(SettingsSection::Permissions, "permissions", panel, 0);
    dialog.SetHostedWidget(SettingsSection::Internet, "internet", panel->DetachInternetGroup(), 0);
    dialog.resize(920, 640);
    dialog.show();
    Settle();
    dialog.ShowSection(SettingsSection::Permissions);
    Settle();
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
        expect(!button->isDefault() && !button->autoDefault(), button->text() + " must not be a default button in Settings");
    auto* pointer = panel->findChild<QCheckBox*>("pointerCheck");
    auto* stop = panel->findChild<QPushButton*>("desktopStopButton");
    expect(pointer && stop && stop->isVisible(), "the Permissions page shows Mouse control and Stop desktop control");
    if (pointer == nullptr)
        return;
    const bool stopped = session.DesktopControlStopped();
    pointer->setFocus(Qt::TabFocusReason);
    expect(pointer->hasFocus(), "Mouse control takes key focus");
    // Checked after each key: a second press of a default Stop button resumes again.
    for (const int key : {Qt::Key_Return, Qt::Key_Enter})
    {
        PressKey(pointer, key);
        expect(session.DesktopControlStopped() == stopped && stop->text() == "Stop desktop control" && !pointer->isChecked() &&
                   !OpenConfirmation(&dialog),
            QStringLiteral("%1 on a switch neither stops desktop control nor changes the switch")
                .arg(key == Qt::Key_Return ? "Return" : "Enter"));
        if (session.DesktopControlStopped() != stopped)
            (void)session.ResumeDesktopControl();
        panel->Refresh();
    }
}

void CheckCapabilityPanel(Expect& expect, revia::runtime::ReviaSession& session)
{
    CapabilityPanel panel(session, {}, nullptr);
    panel.resize(900, 1400);
    panel.show();
    Settle();
    for (const auto& name : names::kUnstyled)
    {
        const std::string text(name.name);
        if (text.ends_with("Check") && text.rfind("profile", 0) != 0)
            expect(panel.findChild<QCheckBox*>(Q(text)) != nullptr, Q(text) + " must be a named CapabilityPanel switch");
    }
    expect(panel.findChild<QPushButton*>("desktopStopButton") != nullptr && panel.findChild<QPushButton*>("stopButton") == nullptr,
        "the emergency button is desktopStopButton, leaving stopButton to the composer");
    expect(panel.findChild<QLabel*>("infoDot") == nullptr && panel.findChild<QLabel*>("permRowHelp") != nullptr,
        "hover-only info dots became visible help lines");
    for (QFrame* row : panel.findChildren<QFrame*>())
    {
        if (row->objectName() == "permRow" || row->objectName() == "permRowNested")
            expect(!row->accessibleDescription().isEmpty() && !row->findChildren<QLabel*>("permHelp").isEmpty(),
                "every permission row has a visible help line and an accessible description");
    }
    for (const QLabel* label : panel.findChildren<QLabel*>())
        expect(!label->text().contains(kGenderedWords), "CapabilityPanel text must be companion-neutral: " + label->text());
    const auto footers = panel.findChildren<QWidget*>("capabilityListFooter");
    expect(footers.size() == 2, "each list editor has its own Unsaved changes footer");
    for (QWidget* footer : footers)
        expect(footer->isHidden(), "list footers stay hidden while nothing is unsaved");
    auto* origins = panel.findChild<QPlainTextEdit*>("browserApprovedOrigins");
    if (origins)
    {
        const QString saved = origins->toPlainText();
        origins->setPlainText("https://example.com");
        Settle();
        expect(std::any_of(footers.begin(), footers.end(), [](QWidget* footer) { return !footer->isHidden(); }),
            "editing a list shows its footer");
        panel.RevertUnsavedLists();
        Settle();
        expect(origins->toPlainText() == saved && std::all_of(footers.begin(), footers.end(), [](QWidget* footer) { return footer->isHidden(); }),
            "RevertUnsavedLists puts the saved list back and hides the footer");
    }
    // While the session has no capability file (it never started) an accepted switch still commits
    // at once; the refusal puts it back off and says why.
    auto* panelStatus = panel.findChild<QLabel*>(QStringLiteral("settingStatus"), Qt::FindDirectChildrenOnly);
    if (auto* process = panel.findChild<QCheckBox*>("processEnabled"))
    {
        process->click();
        QMessageBox* box = OpenConfirmation(&panel);
        if (QAbstractButton* accept = box ? ButtonWithText(box, "Turn on") : nullptr)
            accept->click();
        Settle();
        expect(box && !process->isChecked() && !session.Capabilities().process.enabled && panelStatus &&
                   panelStatus->property("tone").toString() == "error" && !panelStatus->text().isEmpty(),
            "a refused commit puts the switch back off and shows the reason");
    }

    // A capability file in the temporary root, so accepted switches are saved and read back.
    const std::filesystem::path root = std::filesystem::current_path();
    const std::filesystem::path configuration = root / "RuntimeData/Capabilities/capabilities.json";
    std::filesystem::create_directories(configuration.parent_path());
    std::filesystem::create_directories(root / "Audit");
    std::ofstream(configuration) << nlohmann::json{{"mode", "supervised"},
        {"approvedRoots", {revia::actions::PathToUtf8(root)}}, {"createMissingApprovedRoots", false}}.dump();
    std::string loadError;
    const bool loaded = revia::runtime::ReviaSessionTestAccess::Actions(session).Initialize(
        configuration, root / "Audit/settings-fixture.jsonl", loadError);
    expect(loaded, "the fixture capability file must load: " + Q(loadError));
    if (!loaded)
        return;
    panel.Refresh();

    using Capabilities = revia::actions::CapabilitySettings;
    using InputScope = Capabilities::DesktopControl::InputScope;
    // Each risky switch asks before it turns on, with Cancel as the default, and Cancel saves
    // nothing. One that cannot be reached is a failure rather than a skip, so the switches it
    // depends on are turned on first.
    const auto asksFirst = [&](const char* name, const std::function<bool(const Capabilities&)>& saved)
    {
        auto* toggle = panel.findChild<QCheckBox*>(name);
        expect(toggle && toggle->isEnabled() && !toggle->isChecked() && !saved(session.Capabilities()),
            QString(name) + " must be reachable and off before it is turned on");
        if (toggle == nullptr || !toggle->isEnabled())
            return;
        const SettingDescriptor* descriptor = FindSetting(name);
        const QString title = descriptor != nullptr
                                  ? QStringLiteral("Turn on %1?").arg(WithCompanionName(descriptor->label, Q(session.DisplayName())))
                                  : QString();
        toggle->click();
        QMessageBox* box = OpenConfirmation(&panel);
        expect(box && !title.isEmpty() && box->windowTitle() == title && box->defaultButton() == box->button(QMessageBox::Cancel),
            QString(name) + " asks \"" + title + "\" before turning on, with Cancel as the default");
        expect(!toggle->isChecked(), QString(name) + " stays off while the question is open");
        if (box)
            box->button(QMessageBox::Cancel)->click();
        Settle();
        expect(!toggle->isChecked() && !saved(session.Capabilities()), QString(name) + " stays off after Cancel and saves nothing");
    };
    // Accepting saves at once, with no Apply: the list footers stay hidden throughout.
    const auto turnOn = [&](const char* name, const std::function<bool(const Capabilities&)>& saved)
    {
        auto* toggle = panel.findChild<QCheckBox*>(name);
        if (toggle == nullptr || !toggle->isEnabled())
        {
            expect(false, QString(name) + " must be reachable to turn it on");
            return;
        }
        toggle->click();
        if (QMessageBox* box = OpenConfirmation(&panel))
        {
            if (QAbstractButton* accept = ButtonWithText(box, "Turn on"))
                accept->click();
        }
        Settle();
        expect(toggle->isChecked() && saved(session.Capabilities()) &&
                   std::all_of(footers.begin(), footers.end(), [](QWidget* footer) { return footer->isHidden(); }),
            QString(name) + " is saved as soon as it is turned on, without Apply");
    };

    asksFirst("pointerCheck", [](const Capabilities& saved) { return saved.desktopControl.pointer; });
    asksFirst("keyboardCheck", [](const Capabilities& saved) { return saved.desktopControl.keyboard; });
    asksFirst("launchCheck", [](const Capabilities& saved) { return saved.desktopControl.applicationLaunch; });
    asksFirst("processEnabled", [](const Capabilities& saved) { return saved.process.enabled; });
    // Shells and delegated use are grants inside running programs at all.
    for (const char* name : {"processTaskExecution", "processCommandInterpreters"})
    {
        auto* toggle = panel.findChild<QCheckBox*>(name);
        expect(toggle && !toggle->isEnabled(), QString(name) + " must not be reachable while Run approved programs is off");
    }
    // Chosen points, visual targets, command surfaces and unprompted work follow the pointer.
    turnOn("pointerCheck", [](const Capabilities& saved) { return saved.desktopControl.pointer; });
    asksFirst("visualTargetCheck", [](const Capabilities& saved) { return saved.desktopControl.visualTargeting; });
    asksFirst("rawCoordinateCheck", [](const Capabilities& saved) { return saved.desktopControl.rawCoordinates; });
    asksFirst("commandSurfaceCheck", [](const Capabilities& saved) { return saved.desktopControl.allowCommandSurfaces; });
    asksFirst("autonomousDesktopCheck", [](const Capabilities& saved) { return saved.desktopControl.autonomous; });
    // The whole desktop needs chosen points as well.
    turnOn("rawCoordinateCheck", [](const Capabilities& saved) { return saved.desktopControl.rawCoordinates; });
    asksFirst("wholeDesktopCheck", [](const Capabilities& saved) { return saved.desktopControl.scope == InputScope::WholeDesktop; });
    // The browser's risky switches need the interactive browser, which does not ask.
    turnOn("browserEnabled", [](const Capabilities& saved) { return saved.browser.enabled; });
    asksFirst("browserInteract", [](const Capabilities& saved) { return saved.browser.interact; });
    asksFirst("browserTaskInteraction", [](const Capabilities& saved) { return saved.browser.allowTaskInteraction; });
    asksFirst("browserLoopback", [](const Capabilities& saved) { return saved.browser.allowLoopback; });
    turnOn("browserInteract", [](const Capabilities& saved) { return saved.browser.interact; });
    turnOn("processEnabled", [](const Capabilities& saved) { return saved.process.enabled; });
    asksFirst("processTaskExecution", [](const Capabilities& saved) { return saved.process.allowTaskExecution; });
    asksFirst("processCommandInterpreters", [](const Capabilities& saved) { return saved.process.allowCommandInterpreters; });
    turnOn("processCommandInterpreters", [](const Capabilities& saved) { return saved.process.allowCommandInterpreters; });
    // A risky grant inside the browser does not survive the browser being turned off, so turning
    // the browser (which does not ask) back on cannot bring it back unasked.
    if (auto* browser = panel.findChild<QCheckBox*>("browserEnabled"))
    {
        browser->click();
        Settle();
        browser->click();
        Settle();
        auto* interact = panel.findChild<QCheckBox*>("browserInteract");
        expect(browser->isChecked() && !session.Capabilities().browser.interact && interact && !interact->isChecked(),
            "turning the browser off and on again brought Click and fill in forms back without its question");
    }
    turnOn("browserInteract", [](const Capabilities& saved) { return saved.browser.interact; });
    // Turning off never asks and saves at once too.
    for (const char* name : {"browserInteract", "processEnabled"})
    {
        if (auto* toggle = panel.findChild<QCheckBox*>(name))
        {
            toggle->click();
            Settle();
            expect(!toggle->isChecked() && !OpenConfirmation(&panel), QString(name) + " turns off without a question");
        }
    }
    const Capabilities afterOff = session.Capabilities();
    expect(!afterOff.browser.interact && !afterOff.process.enabled, "turning switches off is saved at once");
    expect(!afterOff.process.allowCommandInterpreters, "shells and scripts survived running programs being turned off");

    // A switch still showing a value from before a change made elsewhere (/internet off) commits only
    // its own flag: turning on automatic lookups must not turn internet lookups back on unasked.
    turnOn("internetCheck", [](const Capabilities& saved) { return saved.internet.enabled; });
    const bool internetOff = session.SetInternetAccess(false, false).succeeded;
    if (auto* automatic = panel.findChild<QCheckBox*>("automaticLookupCheck"); automatic && internetOff && automatic->isEnabled())
    {
        automatic->click();
        Settle();
        expect(!OpenConfirmation(&panel) && !session.Capabilities().internet.enabled,
            "a stale internet card turned internet lookups back on without asking");
    }
    else
    {
        expect(false, "the stale internet card fixture could not be set up");
    }

    // Free mode drops each requested task's first approval, so choosing it asks first; Cancel keeps
    // the saved mode, whatever the combo box showed for a moment.
    if (auto* mode = panel.findChild<QComboBox*>(QStringLiteral("taskApprovalMode")))
    {
        using revia::actions::ExecutionMode;
        const ExecutionMode before = session.Capabilities().mode;
        const int free = mode->findData(static_cast<int>(ExecutionMode::OwnerFullAccess));
        mode->setCurrentIndex(free);
        emit mode->activated(free);
        QMessageBox* box = OpenConfirmation(&panel);
        expect(box && box->text() == "Turn on Free mode?" && box->defaultButton() == box->button(QMessageBox::Cancel),
            "choosing Free mode asks first, with Cancel as the default");
        expect(session.Capabilities().mode == before && mode->currentData().toInt() == static_cast<int>(before),
            "the saved mode stays selected while the question is open");
        if (box)
            box->button(QMessageBox::Cancel)->click();
        Settle();
        expect(session.Capabilities().mode == before, "Cancel keeps the saved task mode");
    }

    // Back to everything off, for the captures that follow.
    const bool desktopOff = session.SetDesktopControl(false, false, false, false, false, false, InputScope::ApprovedApplications, false).succeeded;
    revia::browser::BrowserSettings browser = session.Capabilities().browser;
    browser.enabled = false;
    const bool browserOff = session.SetInteractiveBrowser(browser).succeeded;
    panel.Refresh();
    expect(desktopOff && browserOff && !session.Capabilities().desktopControl.AnyEnabled() && !session.Capabilities().browser.enabled,
        "the fixture capability file is back to everything off");
}

void CheckProfilePanel(Expect& expect, revia::runtime::ReviaSession& session)
{
    revia::runtime::ProfileSummary fixture;
    fixture.id = "settingsfixture";
    fixture.displayName = "Fixture";
    fixture.description = "A profile the settings suite saves in its temporary root.";
    fixture.systemPrompt = "You are a test fixture.";
    fixture.memoryEnabled = true;
    const auto saved = session.SaveProfile(fixture);
    expect(saved.succeeded, "the fixture profile must save in the temporary root: " + Q(saved.message));

    ProfilePanel panel(session, nullptr);
    panel.Refresh();
    panel.show();
    Settle();
    for (const char* name : {"profileMemoryCheck", "profileTemperatureCheck", "profileMaxTokensCheck"})
    {
        auto* toggle = panel.findChild<QCheckBox*>(name);
        auto* row = panel.findChild<QFrame*>(QString(name) + "Row");
        expect(toggle && row && row->property("reviaClass").toString() == "settingRow" && !toggle->accessibleName().isEmpty(),
            QString(name) + " must be a labelled switch row");
    }
    for (const QWidget* widget : panel.findChildren<QWidget*>())
    {
        const auto* label = dynamic_cast<const QLabel*>(widget);
        expect(!widget->toolTip().contains(kGenderedWords) && (label == nullptr || !label->text().contains(kGenderedWords)),
            "ProfilePanel text must be companion-neutral");
    }
    if (auto* memory = panel.findChild<QCheckBox*>("profileMemoryCheck"))
        expect(memory->toolTip() == "Applies right away to the active profile; other profiles pick it up when activated.",
            "the memory switch says when it applies");
    auto* combo = panel.findChild<QComboBox*>();
    if (combo)
    {
        const int index = combo->findData(QStringLiteral("settingsfixture"));
        if (index >= 0)
            combo->setCurrentIndex(index);
    }
    Settle();
    expect(!panel.HasUnsavedChanges(), "a freshly loaded profile has no unsaved changes");
    QLineEdit* displayName = nullptr;
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>())
    {
        if (edit->placeholderText() == "Revia")
            displayName = edit;
    }
    if (displayName)
    {
        displayName->setText("Fixture renamed");
        expect(panel.HasUnsavedChanges(), "an edited display name is an unsaved change");
        displayName->setText("Fixture  ");
        expect(!panel.HasUnsavedChanges(), "trailing spaces alone are not an unsaved change");
    }
    if (auto* temperature = panel.findChild<QCheckBox*>("profileTemperatureCheck"))
    {
        temperature->click();
        expect(panel.HasUnsavedChanges(), "turning on a temperature override is an unsaved change");
        temperature->click();
        expect(!panel.HasUnsavedChanges(), "turning it back off is not");
    }
    for (QPushButton* button : panel.findChildren<QPushButton*>())
    {
        if (button->text() == "New profile")
            button->click();
    }
    expect(panel.HasUnsavedChanges(), "a new profile that was never saved is unsaved");
}

void SaveCaptures(Expect& expect, revia::runtime::ReviaSession& session)
{
    const QString directory = qEnvironmentVariable("REVIA_UI_RENDER_DIR");
    if (directory.isEmpty())
        return;
    FakeBackend fake;
    fake.status["speechRecognition.requireWakeWord"] = QStringLiteral("Answers only when it hears: revia, rivia, revya.");
    fake.status["llm.cloud.enabled"] = QStringLiteral("Not set up: llm.cloud.baseUrl is empty.");
    fake.Set("embedding.enabled", true);
    SettingsHooks hooks;
    hooks.companionName = []() { return QStringLiteral("Revia"); };
    hooks.restartApp = []() {};
    hooks.openPanel = [](const char*) {};
    SettingsDialog dialog(fake.Make(), hooks, nullptr);
    auto* panel = new CapabilityPanel(session, {}, nullptr);
    dialog.SetHostedWidget(SettingsSection::Permissions, "permissions", panel, 0);
    dialog.SetHostedWidget(SettingsSection::Internet, "internet", panel->DetachInternetGroup(), 0);
    // What the window's ApplySettingsLayout will do: reflow the panel for the page's width less its margins.
    dialog.SetResizeHook([panel](const QSize size) { panel->ApplyContentWidth(size.width() - 80); });
    dialog.resize(920, 640);
    dialog.show();
    Settle();
    // A pending restart, so the banner is in the capture.
    if (auto* embedding = dialog.findChild<QCheckBox*>("sw_embedding_enabled"))
        embedding->click();
    for (const SettingsSection section : {SettingsSection::General, SettingsSection::Voice, SettingsSection::Models,
             SettingsSection::Permissions, SettingsSection::Integrations})
    {
        dialog.ShowSection(section);
        Settle();
        const QString path = QDir(directory).filePath(QStringLiteral("settings-%1-920x640.png").arg(SectionId(section)));
        expect(dialog.grab().save(path), "the Settings capture must be saved: " + path);
    }
    dialog.findChild<QLineEdit*>("settingsSearch")->setText("micro");
    Settle();
    expect(dialog.grab().save(QDir(directory).filePath("settings-search-920x640.png")), "the search capture must be saved");
}

int RunSettingsSuite(int, char**)
{
    Expect expect;
    CheckCatalog(expect);
    {
        // Never started: it gives the panels something to read, and CheckCapabilityPanel loads a
        // fixture capability file into it.
        revia::runtime::ReviaSession session;
        CheckDialog(expect, session);
        CheckEnterKey(expect, session);
        CheckCapabilityPanel(expect, session);
        CheckProfilePanel(expect, session);
        SaveCaptures(expect, session);
    }
    if (expect.failures == 0)
        std::cout << "Settings dialog suite passed.\n";
    return expect.failures;
}
}

REVIA_DESKTOP_SUITE("settings", RunSettingsSuite);
