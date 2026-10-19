#pragma once

class QWidget;

namespace revia::desktop
{
// Gives every QTabWidget under root (and root itself) the section look: the tab bar is named
// sectionNavigation when it has no name of its own, draws no base line, never scrolls and never
// elides a label. Pages that keep tabs (Mind's views) look the same in every window.
void PolishSectionTabs(QWidget* root);
// Makes page visible wherever it lives: selects it in every QTabWidget and in every QStackedWidget
// marked reviaNavigationStack (the Settings stack) between it and its window, then shows, raises and
// activates that window (the main window, a panel window or the Settings dialog).
void SelectNavigationPage(QWidget* page);
}
