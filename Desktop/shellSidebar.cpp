#include "shellSidebar.h"

#include <QAction>
#include <QBoxLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace revia::desktop
{
namespace
{
constexpr int IconButtonSize = 32;
constexpr int RailButtonSize = 40;

// Icon-only; the text shows only until an icon arrives. tip carries the shortcut, name does not.
QToolButton* IconButton(QWidget* parent, const char* objectName, const QString& name, const QString& tip)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setText(name);
    button->setToolTip(tip);
    button->setAccessibleName(name);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(IconButtonSize, IconButtonSize);
    return button;
}

QLabel* PassiveLabel(QWidget* parent, const char* objectName)
{
    auto* label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    // Clicks and hover belong to the button the label sits on.
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    return label;
}
}

ShellSidebar::ShellSidebar(Callbacks sidebarCallbacks, QWidget* parent) : QWidget(parent), callbacks(std::move(sidebarCallbacks))
{
    setObjectName("navigationSidebar");
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    header = new QWidget(this);
    header->setObjectName("sidebarHeader");
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);
    brandMark = new QLabel(header);
    brandMark->setObjectName("sidebarBrandMark");
    brandMark->setFixedSize(20, 20);
    brandMark->setAlignment(Qt::AlignCenter);
    headerRow->addWidget(brandMark);
    brand = new QLabel("Revia", header);
    brand->setObjectName("sidebarBrand");
    headerRow->addWidget(brand);
    headerRow->addStretch(1);
    toggle = IconButton(header, "sidebarToggle", "Collapse sidebar", "Collapse sidebar (Ctrl+B)");
    connect(toggle, &QToolButton::clicked, this,
        [this]()
        {
            // A narrow window keeps the rail as its default; expanding there lasts only until the
            // window widens, so it is not reported as a preference.
            const bool persisted = !narrow;
            if (narrow)
                temporarilyExpanded = !temporarilyExpanded;
            else
                preferredExpanded = !preferredExpanded;
            ApplyMode();
            if (callbacks.toggled)
                callbacks.toggled(IsExpanded(), persisted);
        });
    headerRow->addWidget(toggle);
    layout->addWidget(header);

    bodyHost = new QWidget(this);
    bodyHost->setObjectName("sidebarBody");
    bodyLayout = new QVBoxLayout(bodyHost);
    bodyLayout->setContentsMargins(0, 4, 0, 0);
    bodyLayout->setSpacing(0);
    layout->addWidget(bodyHost, 1);

    railNewChat = IconButton(this, "sidebarRailNewChat", "New chat", "New chat (Ctrl+N)");
    connect(railNewChat, &QToolButton::clicked, this,
        [this]()
        {
            if (callbacks.newChat)
                callbacks.newChat();
        });
    layout->addWidget(railNewChat);
    railSearch = IconButton(this, "sidebarRailSearch", "Search chats", "Search chats (Ctrl+F)");
    connect(railSearch, &QToolButton::clicked, this,
        [this]()
        {
            if (callbacks.search)
                callbacks.search();
        });
    layout->addWidget(railSearch);
    // A widget rather than a stretch item, so it can leave with the rest of the rail.
    railStretch = new QWidget(this);
    railStretch->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    layout->addWidget(railStretch, 1);
    railPanels = IconButton(this, "sidebarRailPanels", "Panels and more", "Panels, settings and more");
    connect(railPanels, &QToolButton::clicked, this,
        [this]()
        {
            if (callbacks.panels)
                callbacks.panels();
            else if (menu)
                menu->popup(railPanels->mapToGlobal(QPoint(railPanels->width() + 4, 0)), panelsAnchor.data());
        });
    layout->addWidget(railPanels);

    footer = new QWidget(this);
    footer->setObjectName("sidebarFooter");
    footerLayout = new QBoxLayout(QBoxLayout::LeftToRight, footer);
    footerLayout->setContentsMargins(0, 8, 0, 0);
    footerLayout->setSpacing(4);
    menuButton = new QToolButton(footer);
    menuButton->setObjectName("sidebarMenuButton");
    menuButton->setAutoRaise(true);
    menuButton->setFocusPolicy(Qt::TabFocus);
    menuButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(menuButton, &QToolButton::clicked, this, [this]() { PopupMenu(); });
    menuButtonLayout = new QBoxLayout(QBoxLayout::LeftToRight, menuButton);
    menuButtonLayout->setSpacing(8);
    avatar = PassiveLabel(menuButton, "sidebarAvatar");
    avatar->setFixedSize(28, 28);
    avatar->setAlignment(Qt::AlignCenter);
    menuButtonLayout->addWidget(avatar);
    auto* names = new QVBoxLayout;
    names->setSpacing(0);
    accountName = PassiveLabel(menuButton, "sidebarAccountName");
    accountDetail = PassiveLabel(menuButton, "sidebarAccountDetail");
    for (QLabel* label : {accountName, accountDetail})
    {
        // Elided to the room the layout gives them, never wider.
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->installEventFilter(this);
        names->addWidget(label);
    }
    menuButtonLayout->addLayout(names, 1);
    // A badge on the avatar's corner rather than a layout item, so it shows in both modes.
    attentionDot = PassiveLabel(avatar, "sidebarAttentionDot");
    attentionDot->setFixedSize(8, 8);
    attentionDot->move(20, 0);
    attentionDot->setProperty("attention", false);
    attentionDot->hide();
    footerLayout->addWidget(menuButton, 1);
    settingsButton = IconButton(footer, "sidebarSettingsButton", "Settings", "Settings (Ctrl+,)");
    connect(settingsButton, &QToolButton::clicked, this,
        [this]()
        {
            if (callbacks.settings)
                callbacks.settings();
        });
    footerLayout->addWidget(settingsButton);
    layout->addWidget(footer);
    setTabOrder(settingsButton, menuButton);

    SetAccount({}, {});
    ApplyMode();
}

void ShellSidebar::SetBody(QWidget* const widget)
{
    if (body == widget)
        return;
    if (body)
        bodyLayout->removeWidget(body);
    body = widget;
    if (body)
    {
        bodyLayout->addWidget(body, 1);
        body->setVisible(true);
    }
}

void ShellSidebar::SetMenu(QMenu* const value)
{
    menu = value;
}

void ShellSidebar::SetPanelsAnchor(QAction* const action)
{
    panelsAnchor = action;
}

void ShellSidebar::PopupMenu()
{
    if (!menu)
        return;
    // Above the footer button: the button sits at the bottom of the window.
    const QSize size = menu->sizeHint();
    menu->popup(menuButton->mapToGlobal(QPoint(0, -size.height() - 4)));
}

void ShellSidebar::SetAccount(const QString& accountNameText, const QString& accountDetailText)
{
    name = accountNameText.trimmed();
    detail = accountDetailText.trimmed();
    avatar->setText(name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper());
    const QString who = name.isEmpty() ? QStringLiteral("Menu") : detail.isEmpty() ? name : name + QStringLiteral(" · ") + detail;
    menuButton->setToolTip(who + QStringLiteral("\nPanels, settings and more"));
    menuButton->setAccessibleName(name.isEmpty() ? QStringLiteral("Menu") : QStringLiteral("Menu for ") + name);
    ApplyAccountText();
    ApplyMode();
}

void ShellSidebar::ApplyAccountText()
{
    accountName->setText(QFontMetrics(accountName->font()).elidedText(name, Qt::ElideRight, std::max(0, accountName->width())));
    accountDetail->setText(QFontMetrics(accountDetail->font()).elidedText(detail, Qt::ElideRight, std::max(0, accountDetail->width())));
    accountName->setToolTip(name);
}

void ShellSidebar::SetAttention(const bool attention)
{
    attentionDot->setProperty("attention", attention);
    attentionDot->style()->unpolish(attentionDot);
    attentionDot->style()->polish(attentionDot);
    attentionDot->setVisible(attention);
    menuButton->setAccessibleDescription(attention ? QStringLiteral("Activity has warnings or errors") : QString());
}

void ShellSidebar::SetExpanded(const bool expanded)
{
    preferredExpanded = expanded;
    ApplyMode();
}

void ShellSidebar::SetNarrow(const bool value)
{
    if (narrow == value)
        return;
    narrow = value;
    temporarilyExpanded = false;
    ApplyMode();
}

bool ShellSidebar::IsExpanded() const
{
    return narrow ? temporarilyExpanded : preferredExpanded;
}

QToolButton* ShellSidebar::ToggleButton() const
{
    return toggle;
}

void ShellSidebar::SetIconProvider(std::function<QIcon(const char*)> provider)
{
    iconProvider = std::move(provider);
    ApplyIcons();
}

void ShellSidebar::ApplyMode()
{
    const bool expanded = IsExpanded();
    setFixedWidth(expanded ? ExpandedWidth : RailWidth);
    brandMark->setVisible(expanded);
    brand->setVisible(expanded);
    bodyHost->setVisible(expanded);
    for (QWidget* rail :
        {static_cast<QWidget*>(railNewChat), static_cast<QWidget*>(railSearch), railStretch, static_cast<QWidget*>(railPanels)})
        rail->setVisible(!expanded);
    // The rail is a column of 40px targets; expanded, the header and footer buttons are 32px.
    const int button = expanded ? IconButtonSize : RailButtonSize;
    toggle->setFixedSize(button, button);
    settingsButton->setFixedSize(button, button);
    for (QToolButton* rail : {railNewChat, railSearch, railPanels})
        rail->setFixedSize(RailButtonSize, RailButtonSize);
    // Expanded: [avatar name/detail] [gear]. Rail: the gear above the avatar.
    footerLayout->setDirection(expanded ? QBoxLayout::LeftToRight : QBoxLayout::BottomToTop);
    accountName->setVisible(expanded);
    accountDetail->setVisible(expanded && !detail.isEmpty());
    if (expanded)
    {
        menuButton->setMinimumSize(0, 44);
        menuButton->setMaximumSize(QWIDGETSIZE_MAX, 44);
        menuButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        menuButtonLayout->setContentsMargins(6, 4, 6, 4);
    }
    else
    {
        menuButton->setFixedSize(RailButtonSize, RailButtonSize);
        menuButtonLayout->setContentsMargins(6, 6, 6, 6);
    }
    const QString toggleName = expanded ? QStringLiteral("Collapse sidebar") : QStringLiteral("Expand sidebar");
    toggle->setText(toggleName);
    toggle->setAccessibleName(toggleName);
    toggle->setToolTip(toggleName + QStringLiteral(" (Ctrl+B)"));
}

void ShellSidebar::ApplyIcons()
{
    if (!iconProvider)
        return;
    toggle->setIcon(iconProvider("panel-left"));
    railNewChat->setIcon(iconProvider("plus"));
    railSearch->setIcon(iconProvider("search"));
    railPanels->setIcon(iconProvider("grid"));
    settingsButton->setIcon(iconProvider("settings"));
}

void ShellSidebar::RefreshBrandMark()
{
    const QIcon icon = window()->windowIcon();
    if (icon.isNull())
    {
        brandMark->setText(QStringLiteral("●"));
        return;
    }
    brandMark->setPixmap(icon.pixmap(QSize(20, 20)));
}

void ShellSidebar::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        ApplyIcons();
    else if (event->type() == QEvent::WindowIconChange)
        RefreshBrandMark();
}

void ShellSidebar::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    RefreshBrandMark();
}

bool ShellSidebar::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == accountName || watched == accountDetail) && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange))
        ApplyAccountText();
    return QWidget::eventFilter(watched, event);
}
}
