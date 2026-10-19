#pragma once

#include "settingsTypes.h"

#include <QString>

#include <string>
#include <vector>

// The single list of user-facing true/false settings. The Settings dialog builds a row for every
// Dialog-hosted entry, the CapabilityPanel takes its switch labels and help text from the entries
// it hosts, the command palette lists them all and docs/DESKTOP_SETTINGS.md mirrors them. Labels
// and descriptions may hold "{name}", which stands for the active companion's display name.
namespace revia::desktop
{
// Every row, in section and group order. Tool-server switches appear once each as a template
// (IsServerTemplate); the dialog turns a template into one row per configured server.
[[nodiscard]] const std::vector<SettingDescriptor>& SettingCatalog();
// Finds a row by catalog key or by switch objectName. Templates are found by their own key
// ("mcp.servers[*].enabled") or objectName ("sw_mcp_*_enabled"), not by an instantiated one.
[[nodiscard]] const SettingDescriptor* FindSetting(const std::string& keyOrObjectName);

// The sections in the order the dialog lists them.
[[nodiscard]] const std::vector<SettingsSection>& SettingsSections();
[[nodiscard]] QString SectionName(SettingsSection section);
// An icon name from the theme's icon set (design spec 1.6), handed to SettingsHooks::icon.
[[nodiscard]] const char* SectionIcon(SettingsSection section);
// Lower-case id used in objectNames: settingsSection_<id>.
[[nodiscard]] const char* SectionId(SettingsSection section);

// Tool-server rows: the template's key holds "[*]" and its objectName and label hold "*". A
// server row replaces them with the server's id (the identifier configManager reads), sanitised to
// [A-Za-z0-9_] in the objectName.
[[nodiscard]] bool IsServerTemplate(const SettingDescriptor& descriptor);
[[nodiscard]] SettingDescriptor ServerRow(const SettingDescriptor& serverTemplate, const std::string& serverId);
[[nodiscard]] std::string ObjectNamePart(const std::string& text);

// "{name}" replaced by the companion's display name ("Revia" when none is known yet).
[[nodiscard]] QString WithCompanionName(QString text, const QString& companionName);
// The question a risky switch asks before the change it confirms, as a sentence the owner can
// answer ("Let {name} see your screen?"), with "{name}" left for the caller to fill. A row
// without its own question asks Turn on (or off) "label"?.
[[nodiscard]] QString ConfirmQuestion(const SettingDescriptor& descriptor, bool turningOn);
// The description to show while the switch is off, for the few rows whose off state needs its own
// sentence (closing the window quits Revia). Empty when the ordinary description applies.
[[nodiscard]] QString DescriptionWhenOff(const SettingDescriptor& descriptor);
}
