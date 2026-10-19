#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

class QAbstractButton;
class QAction;
class QFocusEvent;
class QLabel;
class QMenu;
class QWidget;

// Light and dark appearance for the desktop: the colour tokens, the stylesheet built from them,
// the application palette, icons and native window chrome. A plain module, not a QObject: state
// lives in this translation unit and callers that bake colours in (painted widgets, rendered
// HTML) register a listener and redraw when the scheme changes.
//
// The preference is System, Light or Dark. System follows Windows' "app mode" setting and changes
// live; REVIA_THEME=system|light|dark overrides the stored preference when the window starts (the
// tests set it so captures never depend on the machine). Windows high contrast wins over both: the
// custom colours step aside and the system palette is used.
namespace revia::desktop::theme
{
enum class Preference
{
    System,
    Light,
    Dark
};

enum class Scheme
{
    Light,
    Dark
};

// One value per colour role. Names match the {{token}} placeholders in reviaTheme.qss.
struct Tokens
{
    Scheme scheme = Scheme::Light;
    QColor background, sidebar, sidebarHover, sidebarSelected, surface, surfaceAlt, surfaceHover, inputBackground, border,
        borderStrong, inputBorder, divider, text, textSecondary, textTertiary, textDisabled, accent, accentHover, accentPressed,
        accentSoft, textOnAccent, selection, selectionText, focusRing, link, danger, dangerSoft, warning, warningSoft, success,
        successSoft, userBubble, codeBackground, codeBorder, scrollbar, scrollbarHover, switchOff, scrim, shadow;
};

// Status tones for text and glyphs that carry meaning; RoleColor maps them to the current tokens.
enum class Role
{
    Accent,
    Warning,
    Danger,
    Success,
    TextSecondary
};

// A foreground/background pair the stylesheet or a painted widget uses, with the WCAG contrast
// ratio it must reach (4.5 for text, 3 for boundaries and glyphs).
struct ContrastPair
{
    const char* name;
    QColor fg, bg;
    double minimum;
};

// Sets the Fusion style and the app font once, reads the preference (REVIA_THEME, then QSettings
// appearance/theme), applies it to root and starts following the system scheme. Safe to call for
// every window; later calls only style the new root.
void Install(QWidget* root);

// Stores the preference (QSettings appearance/theme) and applies it at once, whatever REVIA_THEME
// said at startup.
void SetPreference(Preference preference);
[[nodiscard]] Preference CurrentPreference();
[[nodiscard]] Scheme CurrentScheme();
[[nodiscard]] Scheme SystemScheme();
[[nodiscard]] bool HighContrastActive();

// The tokens in effect. Under high contrast they are derived from the system palette instead.
[[nodiscard]] const Tokens& Current();
[[nodiscard]] const Tokens& Light();
[[nodiscard]] const Tokens& Dark();
[[nodiscard]] QColor RoleColor(Role role);

// Runs after every scheme change, once the palette and stylesheet are in place.
int AddListener(std::function<void()> listener);
void RemoveListener(int id);

// revia.qss (structure) plus reviaTheme.qss filled from the current tokens; under high contrast,
// revia.qss plus reviaContrast.qss, whose few colours all come from the system palette. The
// overload fills the template from any token set.
[[nodiscard]] QString StyleSheet();
[[nodiscard]] QString StyleSheet(const Tokens& tokens);
// Default stylesheet for transcript documents (QTextDocument::setDefaultStyleSheet).
[[nodiscard]] QString DocumentCss();

// Line icons on a 24-unit grid (see IconNames()). A suffix picks the tone: "mic" is secondary text
// that turns accent when checked, "mic:accent", "cursor:danger" and "arrow-up:onAccent" (for
// accent-filled buttons) keep one tone. Unknown names give a null icon.
[[nodiscard]] QIcon Icon(const char* name, int logicalSize = 18);
[[nodiscard]] std::vector<const char*> IconNames();
// Sets the icon now and again after every scheme change, for as long as the button or action lives.
void BindIcon(QAbstractButton* button, const char* name, int size = 18);
void BindIcon(QAction* action, const char* name);
// Renders inline SVG elements (24-unit grid, stroked) into label in the role's colour, and again
// after every scheme change.
void BindSvgLabel(QLabel* label, const QString& svgPaths, Role role, int size);

// Whether a keyboard focus ring should show after this focus event, given whether one showed
// before. Focus from Tab, Backtab or a shortcut shows it and a click hides it; deactivating and
// reactivating the window, opening and closing a popup, or a programmatic setFocus() leave it as
// it was, so a ring the keyboard put there survives an Alt+Tab. ToggleSwitch uses it, and so does
// the theme for item views, which key their ring off the kbdFocus property.
[[nodiscard]] bool KeyboardFocusAfter(bool before, const QFocusEvent& event);

// Native rounding and shadow for menus on Windows 11 (no-op elsewhere).
void PolishMenu(QMenu* menu);
// Dark or light caption, and on Windows 11 caption, text and border colours, for a top-level window.
// Under high contrast it hands those colours back to the system.
void ApplyNativeFrame(QWidget* topLevel);

// The opacity reviaTheme.qss gives every progress-bar fill ({{accent/40}} and the like), so the
// centred percentage stays at AA over the fill; ContrastPairs checks text over each fill at it.
inline constexpr int ProgressFillPercent = 40;

[[nodiscard]] double ContrastRatio(const QColor& first, const QColor& second);
[[nodiscard]] std::vector<ContrastPair> ContrastPairs(const Tokens& tokens);

[[nodiscard]] QString ToString(Preference preference);
[[nodiscard]] Preference PreferenceFromString(const QString& text);

// Forces the high-contrast state for tests; nullopt returns to the system's setting.
void SetHighContrastForTest(std::optional<bool> active);
}
