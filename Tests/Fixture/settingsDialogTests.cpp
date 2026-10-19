#include "capabilityPanel.h"
#include "desktopObjectNames.h"
#include "desktopSuites.h"
#include "profilePanel.h"
#include "settingsCatalog.h"
#include "settingsDialog.h"

#include "Core/preferenceStore.h"
#include "Core/settingsFileKeys.h"
#include "Runtime/reviaSession.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QComboBox>
#include <QIcon>
#include <QPalette>
#include <QPixmap>

#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Revia.SettingsDialog: the settings catalog against the settings inventory, and the Settings
// dialog, CapabilityPanel and ProfilePanel behaviour, all through a fake backend whose writes
// finish synchronously unless told to wait. Nothing here writes a preference, the owner settings
// overlay, a capability or the virtual machine file: the only real session is never started, and
// the capability confirmations it shows are answered Cancel.
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
            const SettingDescriptor* row = CatalogRow(key.key);
            if (row == nullptr || row->apply == SettingApply::Live)
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
            const SettingDescriptor* row = CatalogRow(key.key);
            Set(key.key, row != nullptr && row->defaultValue);
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

void PressKey(QWidget* target, const int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(target, &release);
    Settle();
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
            expect(box->text().contains("speaks as you") && box->text().contains("change this back"), "the warning explains the risk");
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
    if (auto* vision = dialog.findChild<QCheckBox*>(QStringLiteral("sw_vision_enabled")))
    {
        fake.deferNext = true;
        vision->click();
        Settle();
        const QLabel* status = RowLabel(&dialog, "sw_vision_enabledRow", "settingStatus");
        expect(!vision->isEnabled() && status && !status->isHidden() && status->text().startsWith("Saving"),
            "a pending write disables the switch and says Saving");
        if (fake.deferred)
            fake.deferred({true, QStringLiteral("Vision is on.")});
        Settle();
        expect(vision->isEnabled() && vision->isChecked() && status && status->text() == "Vision is on." &&
                   status->property("tone").toString() == "success",
            "the switch comes back with the backend's message when done runs");
        expect(!changed.empty() && changed.back() == std::pair<std::string, bool>{"vision.enabled", true},
            "valueChanged runs after a successful write");
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
    }

    // Companion name substitution.
    const QLabel* bargeIn = RowLabel(&dialog, "bargeInCheckRow", "settingTitle");
    expect(bargeIn && bargeIn->text() == "Let me interrupt Revia", "{name} becomes the companion's name");
    dialog.SetCompanionName("Nova");
    expect(bargeIn && bargeIn->text() == "Let me interrupt Nova", "SetCompanionName re-renders the rows");
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
    auto* noResults = dialog.findChild<QLabel*>(QStringLiteral("settingsNoResults"));
    expect(noResults && noResults->isHidden(), "the no-results line stays hidden while something matches");
    search->setText("zzqx");
    Settle();
    expect(noResults && !noResults->isHidden() && noResults->text().contains("zzqx") && stack->isHidden(),
        "a query with no hits says so");
    search->setFocus();
    PressKey(search, Qt::Key_Escape);
    expect(search->text().isEmpty() && dialog.isVisible() && micRow && !micRow->isHidden() && speechRow && !speechRow->isHidden(),
        "Esc clears the search first and restores every row");
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

    // Every section fits 760x540 without horizontal scrolling.
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
    dialog.resize(920, 640);

    // Hosting the real panels: the capability switches become reachable and searchable.
    auto* panel = new CapabilityPanel(session, {}, nullptr);
    dialog.SetHostedWidget(SettingsSection::Permissions, "permissions", panel, 0);
    QWidget* internetGroup = panel->DetachInternetGroup();
    expect(internetGroup && internetGroup->findChild<QCheckBox*>("internetCheck") && internetGroup->findChild<QCheckBox*>("browserEnabled"),
        "the detached internet group holds the internet and browser switches");
    expect(panel->findChild<QCheckBox*>("internetCheck") == nullptr && panel->DetachInternetGroup() == internetGroup,
        "after detaching, the panel no longer lays the group out and returns the same container");
    dialog.SetHostedWidget(SettingsSection::Internet, "internet", internetGroup, 0);
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
    // A risky switch asks before anything is written; answering Cancel keeps it off.
    if (auto* pointer = panel.findChild<QCheckBox*>("pointerCheck"))
    {
        expect(!pointer->isChecked() && pointer->isEnabled(), "Mouse control starts off");
        pointer->click();
        QMessageBox* box = OpenConfirmation(&panel);
        expect(box && box->windowTitle() == "Turn on Mouse control?" && box->defaultButton() == box->button(QMessageBox::Cancel),
            "turning on Mouse control asks, with Cancel as the default");
        expect(!pointer->isChecked(), "the switch stays off while the question is open");
        if (box)
            box->button(QMessageBox::Cancel)->click();
        Settle();
        expect(!pointer->isChecked() && !session.Capabilities().desktopControl.pointer, "Cancel writes nothing");
    }
    for (const char* name : {"processEnabled", "browserInteract", "commandSurfaceCheck"})
    {
        auto* toggle = panel.findChild<QCheckBox*>(name);
        if (toggle == nullptr || !toggle->isEnabled())
            continue;
        toggle->click();
        QMessageBox* box = OpenConfirmation(&panel);
        expect(box && box->windowTitle().startsWith("Turn on "), QString(name) + " asks before turning on");
        if (box)
            box->button(QMessageBox::Cancel)->click();
        Settle();
        expect(!toggle->isChecked(), QString(name) + " stays off after Cancel");
    }
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
        // Never started: it only gives the panels something to read.
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
