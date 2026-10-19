#include "tabNavigation.h"

#include "desktopObjectNames.h"
#include "settingsCatalog.h"
#include "settingsDialog.h"

#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>

namespace revia::desktop
{
namespace
{
void PolishTabs(QTabWidget* tabs)
{
    QTabBar* bar = tabs->tabBar();
    // QTabWidget names its own bar; any other name was given on purpose and is kept.
    if (bar->objectName().isEmpty() || bar->objectName() == QLatin1String("qt_tabwidget_tabbar"))
        bar->setObjectName("sectionNavigation");
    bar->setDrawBase(false);
    bar->setUsesScrollButtons(false);
    bar->setElideMode(Qt::ElideNone);
}
}

void PolishSectionTabs(QWidget* root)
{
    if (root == nullptr)
        return;
    if (auto* tabs = qobject_cast<QTabWidget*>(root))
        PolishTabs(tabs);
    for (QTabWidget* tabs : root->findChildren<QTabWidget*>())
        PolishTabs(tabs);
}

void SelectNavigationPage(QWidget* page)
{
    if (page == nullptr)
        return;
    QWidget* child = page;
    for (QWidget* ancestor = page->parentWidget(); ancestor != nullptr; child = ancestor, ancestor = ancestor->parentWidget())
    {
        if (auto* tabs = qobject_cast<QTabWidget*>(ancestor))
        {
            for (int index = 0; index < tabs->count(); ++index)
            {
                QWidget* candidate = tabs->widget(index);
                if (candidate == page || candidate->isAncestorOf(page))
                {
                    tabs->setCurrentIndex(index);
                    break;
                }
            }
        }
        else if (auto* stack = qobject_cast<QStackedWidget*>(ancestor);
                 stack != nullptr && stack->property(names::kNavigationStackProperty).toBool() && stack->indexOf(child) >= 0)
        {
            stack->setCurrentWidget(child);
        }
        if (ancestor->isWindow())
            break;
    }
    QWidget* window = page->window();
    // The Settings dialog keeps its section list in step with its stack; going through it selects
    // the list row as well.
    if (auto* settings = dynamic_cast<SettingsDialog*>(window))
    {
        for (const SettingsSection section : SettingsSections())
        {
            QWidget* sectionPage = settings->SectionPage(section);
            if (sectionPage == page || (sectionPage != nullptr && sectionPage->isAncestorOf(page)))
            {
                settings->ShowSection(section);
                break;
            }
        }
    }
    if (window->isMinimized())
        window->showNormal();
    else
        window->show();
    window->raise();
    window->activateWindow();
}
}
