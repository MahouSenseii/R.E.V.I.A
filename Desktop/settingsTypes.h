#pragma once

#include <QIcon>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Types shared by the settings catalog and dialog, the session adapter that reads and writes the
// stores, and the window that wires them together. The dialog never sees ReviaSession: everything
// it persists outside QSettings goes through a SettingsBackend, and everything it asks of the
// window goes through SettingsHooks.
namespace revia::desktop
{
enum class SettingsSection
{
    General,
    Appearance,
    Chat,
    Voice,
    Vision,
    Internet,
    Memory,
    Models,
    Permissions,
    Integrations,
    Advanced
};

// Where a value is kept: QSettings, the companion's preference store, the owner settings overlay
// (shared by every companion), the capability file, the running session only, the active profile,
// or the companion's virtual machine settings file.
enum class SettingStore
{
    Shell,
    Preference,
    SettingsFile,
    Capability,
    Session,
    Profile,
    VirtualMachine
};

enum class SettingApply
{
    Live,
    Restart,
    LiveWhenOffRestartWhenOn,
    Partial
};

enum class SettingRisk
{
    Normal,
    Privacy,
    Dangerous
};

enum class ConfirmWhen
{
    Never,
    TurningOn,
    TurningOff
};

// The widget that owns the switch: a SettingsDialog row, or an existing panel's own toggle.
enum class SettingHost
{
    Dialog,
    CapabilityPanel,
    ProfilePanel
};

// Every member has a default, so a catalog row written with designated initializers names only
// what it sets and still compiles without -Wmissing-field-initializers.
struct SettingDescriptor
{
    std::string key{}, objectName{};
    SettingsSection section = SettingsSection::General;
    QString group{}, label{}, description{}, warning{};
    SettingStore store = SettingStore::Shell;
    SettingApply apply = SettingApply::Live;
    SettingRisk risk = SettingRisk::Normal;
    ConfirmWhen confirm = ConfirmWhen::Never;
    SettingHost host = SettingHost::Dialog;
    std::vector<std::string> requiresAll{};
    std::string disabledWhenOn{}, softRequires{};
    bool allCompanions = false;
    // The switch shows the opposite of the stored value (image.offline is "Let picture generation use the internet").
    bool inverted = false;
    bool autonomy = false;
    bool defaultValue = false;
    QStringList keywords{};
};

struct SettingKeyRef
{
    std::string key;
    SettingStore store = SettingStore::Shell;
};

// value is what is persisted; running is what is in effect now. They differ for a Restart setting
// that has been changed since startup, and the restart banner counts those rows.
struct SettingRead
{
    bool available = false;
    bool value = false;
    bool running = false;
    QString unavailableReason;
};

struct SettingWrite
{
    bool succeeded = false;
    QString message;
};

using SettingDone = std::function<void(SettingWrite)>;

// Every store except Shell, which the dialog handles itself through QSettings. Writes are
// asynchronous and call done exactly once, on the UI thread: the row stays busy until it runs. A
// backend that has to drop answers (their session was retired) calls
// SettingsDialog::AbandonPendingWrites() instead, after which a late answer is ignored.
struct SettingsBackend
{
    std::function<std::map<std::string, SettingRead>(const std::vector<SettingKeyRef>&)> readAll; // UI thread, fast
    std::function<void(const SettingKeyRef&, bool, SettingDone)> write;
    std::function<void(const SettingKeyRef&, SettingDone)> reset;
    std::function<QString(const std::string& key)> readChoice;
    std::function<void(const std::string& key, const QString& value, SettingDone)> writeChoice;
    std::function<double(const std::string& field)> readProfileValue; // "temperature" | "maxTokens"
    std::function<void(const std::string& field, double, SettingDone)> writeProfileValue;
    std::function<std::vector<std::string>()> mcpServers;
    std::function<QString(const std::string& key)> statusText;
};

struct SettingsHooks
{
    std::function<void(const std::string& key, bool value)> valueChanged;
    std::function<int()> themePreference; // 0 System, 1 Light, 2 Dark
    std::function<void(int)> setThemePreference;
    std::function<int()> chatTextSize; // 14, 15 or 17
    std::function<void(int)> setChatTextSize;
    std::function<void(const char* panelKey)> openPanel;
    std::function<void()> restartApp;
    std::function<void()> onHide;
    std::function<QIcon(const char* iconName)> icon;
    std::function<QString()> companionName;
    std::function<bool()> trayAvailable;
};
}
