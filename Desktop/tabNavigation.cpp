#include "tabNavigation.h"

#include <QTabBar>
#include <QTabWidget>

#include <initializer_list>

namespace revia::desktop
{
namespace
{
struct PageLink
{
    const char* name;
    const char* title;
};

void AddSection(QTabWidget* tabs, const int position, const char* name, const char* title, const std::initializer_list<PageLink> pages)
{
    auto* section = new QTabWidget(tabs);
    section->setObjectName(name);
    section->setDocumentMode(true);
    for (const auto& link : pages)
    {
        for (int index = 0; index < tabs->count(); ++index)
        {
            QWidget* page = tabs->widget(index);
            if (page->objectName() != link.name)
                continue;
            const QString description = QString(tabs->tabText(index)).replace("&&", "&");
            tabs->removeTab(index);
            const int added = section->addTab(page, link.title);
            section->setTabToolTip(added, description);
            break;
        }
    }
    tabs->insertTab(position, section, title);
}
}

void GroupNavigationPages(QTabWidget* tabs)
{
    if (tabs == nullptr)
        return;
    if (tabs->findChild<QTabWidget*>("workspaceTabs") == nullptr)
    {
        AddSection(tabs, 1, "workspaceTabs", "Workspace", {{"canvasPage", "Canvas"}, {"visionTab", "Vision"}});
        AddSection(tabs, 2, "companionTabs", "Companion",
            {{"profilesTab", "Profiles"}, {"mindTab", "Mind"}, {"memoryTab", "Memory"}, {"voiceTab", "Voice"}, {"presencePage", "Presence"},
                {"audienceStudioScroll", "Audience"}});
        AddSection(tabs, 3, "studioTabs", "Studio",
            {{"agentStudioScroll", "Agents"}, {"learningStudioScroll", "Learning"}, {"developmentStudioScroll", "Development"}});
        AddSection(tabs, 5, "settingsTabs", "Settings", {{"settingsPage", "General"}, {"permissionsPage", "Permissions"}});
        tabs->setCurrentIndex(0);
    }
    tabs->tabBar()->setObjectName("primaryNavigation");
    for (auto* group : tabs->findChildren<QTabWidget*>())
    {
        group->tabBar()->setObjectName("sectionNavigation");
        group->tabBar()->setDrawBase(false);
        group->tabBar()->setUsesScrollButtons(false);
        group->tabBar()->setElideMode(Qt::ElideNone);
    }
    tabs->tabBar()->setUsesScrollButtons(false);
    tabs->tabBar()->setElideMode(Qt::ElideNone);
}

void SelectNavigationPage(QWidget* page)
{
    if (page == nullptr)
        return;
    for (QWidget* ancestor = page->parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget())
    {
        auto* group = qobject_cast<QTabWidget*>(ancestor);
        if (group == nullptr)
            continue;
        for (int index = 0; index < group->count(); ++index)
        {
            QWidget* candidate = group->widget(index);
            if (candidate == page || candidate->isAncestorOf(page))
            {
                group->setCurrentIndex(index);
                break;
            }
        }
    }
}
}
