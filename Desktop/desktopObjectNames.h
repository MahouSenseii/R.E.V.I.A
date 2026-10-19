#pragma once

#include <array>
#include <string_view>

// Every objectName the redesigned shell, panel windows and Settings dialog introduce, except the
// per-setting switches, which take theirs from SettingCatalog(). The code that builds a widget, the
// stylesheet that paints it and the tests that find it all read the name from here, so a name that
// is missing gets no rule and is not invented elsewhere. Names the redesign keeps unchanged
// (chatHistory, messageInput, chatSessionList, ...) stay where they are today.
namespace revia::desktop::names
{
// alwaysPresent: the widget exists under ReviaWindow once BuildShell has run. It is false for
// widgets made on demand, and for names inside CapabilityPanel or ProfilePanel, which are rebuilt
// for every session.
struct Name
{
    std::string_view name;
    bool alwaysPresent;
};

// Each of these has its own #name rule in the stylesheet, in both schemes (Revia.Theme checks it).
inline constexpr std::array kStyled = {
    // shell (C/D)
    Name{"rootPanel", true}, Name{"navigationSidebar", true}, Name{"sidebarHeader", true}, Name{"sidebarBrand", true},
    Name{"sidebarBrandMark", true}, Name{"sidebarToggle", true}, Name{"sidebarBody", true}, Name{"sidebarRailNewChat", true},
    Name{"sidebarRailSearch", true}, Name{"sidebarRailPanels", true}, Name{"sidebarFooter", true}, Name{"sidebarMenuButton", true},
    Name{"sidebarAvatar", true}, Name{"sidebarAccountName", true}, Name{"sidebarAccountDetail", true},
    Name{"sidebarAttentionDot", true}, Name{"sidebarSettingsButton", true}, Name{"sidebarMenu", true},
    Name{"chatColumn", true}, Name{"chatHeader", true}, Name{"chatTitle", true}, Name{"privacyIndicators", true},
    Name{"privacyMic", true}, Name{"privacyPerception", true}, Name{"privacyScreen", true}, Name{"privacyCamera", false},
    Name{"privacyDesktop", true}, Name{"headerDesktopStop", true}, Name{"statePill", true}, Name{"stateDot", true},
    Name{"stateLabel", true}, Name{"stateDetailLabel", true}, Name{"chatMoreButton", true}, Name{"chatMoreMenu", true},
    Name{"statusPopover", true}, Name{"statusPopoverActivity", true}, Name{"jumpToLatest", true},
    Name{"composerArea", true}, Name{"composerShadowHost", true}, Name{"composerCard", true}, Name{"composerContext", true},
    Name{"composerContextLabel", true}, Name{"composerContextAction", true}, Name{"composerToolbar", true},
    Name{"composerAddMenu", true}, Name{"composerProfileChip", true}, Name{"composerProfileMenu", true},
    Name{"composerHandsFreeChip", true}, Name{"toast", true}, Name{"toastLabel", true}, Name{"navigationLockOverlay", true},
    Name{"commandPaletteScrim", true}, Name{"commandPalette", true}, Name{"commandPaletteInput", true},
    Name{"commandPaletteList", true}, Name{"commandPaletteEmpty", true}, Name{"commandPaletteFooter", true},
    Name{"panelHeader", true}, Name{"panelPlaceBeside", true}, Name{"panelOpenSettings", false},
    Name{"shortcutsDialog", false}, Name{"shortcutsTable", false}, Name{"presenceOpenSettings", true}, Name{"microphoneRow", true},
    // settings (B2)
    Name{"settingsDialog", true}, Name{"settingsNav", true}, Name{"settingsSearch", true}, Name{"settingsSections", true},
    Name{"settingsStack", true}, Name{"settingsSectionTitle", true}, Name{"settingsSectionIntro", true},
    Name{"settingsGroupTitle", true}, Name{"settingsGroup", true}, Name{"settingsDivider", true},
    Name{"settingsRiskyHeader", true}, Name{"settingTitle", true}, Name{"settingDescription", true},
    Name{"settingBadge", true}, Name{"settingWarning", true}, Name{"settingStatus", true},
    Name{"settingsRestartBanner", true}, Name{"settingsRestartNow", true}, Name{"settingsRestartLater", true},
    Name{"settingsNoResults", true}, Name{"settingConfirmDialog", false},
    Name{"themeSegment", true}, Name{"themeSystem", true}, Name{"themeLight", true}, Name{"themeDark", true},
    Name{"chatTextSizeCombo", true}, Name{"cloudUseForCombo", true}, Name{"profileTemperatureSpin", true},
    Name{"profileMaxTokensSpin", true}, Name{"permRow", false}, Name{"permRowHelp", false}, Name{"capabilityListFooter", false},
    Name{"desktopStopButton", false}, Name{"stopBar", false},
};

// Names that code and tests find widgets by but that need no #name rule: the stylesheet reaches
// them through their type, kStyledRoles or kStyledClasses.
inline constexpr std::array kUnstyled = {
    // CapabilityPanel switches that had no name (B2); the settings catalog refers to them
    Name{"internetCheck", false}, Name{"automaticLookupCheck", false}, Name{"visibleBrowserCheck", false},
    Name{"autonomousResearchCheck", false}, Name{"cameraCheck", false}, Name{"autonomousCameraCheck", false},
    Name{"pointerCheck", false}, Name{"keyboardCheck", false}, Name{"launchCheck", false}, Name{"rawCoordinateCheck", false},
    Name{"visualTargetCheck", false}, Name{"wholeDesktopCheck", false}, Name{"commandSurfaceCheck", false},
    Name{"autonomousDesktopCheck", false},
    // ProfilePanel switches (B2)
    Name{"profileMemoryCheck", false}, Name{"profileTemperatureCheck", false}, Name{"profileMaxTokensCheck", false},
    // settings rows that hold no catalog switch: B2's Appearance rows, then the custom, info and link rows D adds
    Name{"themeSegmentRow", true}, Name{"chatTextSizeComboRow", true}, Name{"initiativeMaxSpinRow", true},
    Name{"hardFilterStatusRow", true}, Name{"microphoneDeviceRow", true}, Name{"voiceDeviceLinkRow", true},
    Name{"cameraSnapshotsLinkRow", true}, Name{"cloudUseForComboRow", true}, Name{"resourceSampleSpinRow", true},
};

// Widgets that repeat with per-instance objectNames are styled by dynamic property instead of #name.
inline constexpr std::array kStyledRoles = { // QSS: [reviaRole="x"]
    std::string_view{"settingsSection"}, std::string_view{"panelWindow"}, std::string_view{"settingSwitch"}};
inline constexpr std::array kStyledClasses = { // QSS: [reviaClass="x"]
    std::string_view{"settingRow"}};

// Dynamic properties shared across slices. reviaNavigationStack=true marks a QStackedWidget that
// SelectNavigationPage switches to the page it is asked for (the Settings stack).
inline constexpr char kRoleProperty[] = "reviaRole";
inline constexpr char kClassProperty[] = "reviaClass";
inline constexpr char kNavigationStackProperty[] = "reviaNavigationStack";

// Per-instance names are a fixed prefix plus a key, or a switch's objectName plus a suffix.
inline constexpr std::string_view kPanelWindowPrefix = "panelWindow_";             // + PanelSpec::key
inline constexpr std::string_view kSettingsSectionPrefix = "settingsSection_";     // + SectionId(section)
inline constexpr std::string_view kSettingsOpenPanelPrefix = "settingsOpenPanel_"; // + PanelSpec::key
inline constexpr std::string_view kSettingRowSuffix = "Row";                       // switch objectName + "Row"
}
