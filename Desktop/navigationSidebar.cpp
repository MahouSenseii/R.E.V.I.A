#include "navigationSidebar.h"

#include <QByteArray>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSizePolicy>
#include <QString>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace
{
QIcon NavigationIcon(const QString& paths)
{
    QIcon icon;
    for (const auto state : {QIcon::Off, QIcon::On})
    {
        const QString color = state == QIcon::On ? "#89efdb" : "#b0c1d8";
        const QByteArray svg =
            QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
                    "<g fill='none' stroke='%1' stroke-width='1.6' stroke-linecap='round' stroke-linejoin='round'>%2</g></svg>")
                .arg(color, paths)
                .toUtf8();
        QSvgRenderer renderer(svg);
        QPixmap pixmap(48, 48);
        pixmap.setDevicePixelRatio(2);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter, QRectF(0, 0, 24, 24));
        painter.end();
        icon.addPixmap(pixmap, QIcon::Normal, state);
    }
    return icon;
}
}

NavigationSidebar::NavigationSidebar(QTabWidget* pages, std::function<void()> layoutChanged, QWidget* parent)
    : QWidget(parent), pages(pages), layoutChanged(std::move(layoutChanged))
{
    setObjectName("navigationSidebar");
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 12, 8, 12);
    layout->setSpacing(8);
    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    brand = new QLabel("REVIA", this);
    brand->setObjectName("sidebarBrand");
    top->addWidget(brand, 1);
    toggle = new QToolButton(this);
    toggle->setObjectName("sidebarToggle");
    toggle->setIcon(NavigationIcon("<rect x='3' y='4' width='18' height='16' rx='3'/><path d='M9 4v16'/><path d='m15 9-3 3 3 3'/>"));
    toggle->setIconSize(QSize(20, 20));
    toggle->setFocusPolicy(Qt::StrongFocus);
    top->addWidget(toggle);
    layout->addLayout(top);
    layout->addSpacing(12);
    heading = new QLabel("Navigate", this);
    heading->setObjectName("sidebarHeading");
    layout->addWidget(heading);
    const std::array<const char*, 6> paths{
        "<path d='M5 4h14a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H9l-6 3V6a2 2 0 0 1 2-2Z'/><path d='M7 9h10M7 13h6'/>",
        "<rect x='3' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='3' width='7' height='7' rx='1.5'/>"
        "<rect x='3' y='14' width='7' height='7' rx='1.5'/><rect x='14' y='14' width='7' height='7' rx='1.5'/>",
        "<circle cx='12' cy='7' r='4'/><path d='M4 21v-2a8 8 0 0 1 16 0v2'/>",
        "<circle cx='12' cy='5' r='3'/><circle cx='5' cy='19' r='3'/><circle cx='19' cy='19' r='3'/><path d='m10.5 8-4 8m7-8 4 8M8 19h8'/>",
        "<path d='M2 12h4l3-8 6 16 3-8h4'/>",
        "<circle cx='12' cy='12' r='4'/><path d='M12 2v3m0 14v3M2 12h3m14 0h3M5 5l2 2m10 10 2 2M5 19l2-2M17 7l2-2'/>"};
    for (int index = 0; index < pages->count(); ++index)
    {
        if (index == pages->count() - 1)
            layout->addStretch(1);
        auto* button = new QToolButton(this);
        button->setObjectName(QString("sidebarPage%1").arg(index));
        button->setIcon(NavigationIcon(paths.at(static_cast<std::size_t>(index))));
        button->setIconSize(QSize(20, 20));
        button->setCheckable(true);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(38);
        connect(button, &QToolButton::clicked, this,
            [this, index]()
            {
                if (this->pages->isEnabled())
                    this->pages->setCurrentIndex(index);
                Refresh();
            });
        buttons.push_back(button);
        layout->addWidget(button);
    }
    connect(toggle, &QToolButton::clicked, this,
        [this]()
        {
            userCollapsed = !userCollapsed;
            ApplyMode();
            this->layoutChanged();
        });
    connect(pages, &QTabWidget::currentChanged, this, [this]() { Refresh(); });
    pages->installEventFilter(this);
    pages->tabBar()->hide();
    setEnabled(pages->isEnabled());
    ApplyMode();
    Refresh();
}

void NavigationSidebar::SetNarrow(const bool value)
{
    if (narrow == value)
        return;
    narrow = value;
    ApplyMode();
}

void NavigationSidebar::ApplyMode()
{
    const bool compact = narrow || userCollapsed;
    setFixedWidth(compact ? 56 : 184);
    brand->setVisible(!compact);
    heading->setVisible(!compact);
    toggle->setToolTip(narrow ? "Sidebar is compact at this window width" : compact ? "Expand sidebar" : "Collapse sidebar");
    toggle->setAccessibleName(toggle->toolTip());
    toggle->setEnabled(!narrow);
    for (auto* button : buttons)
        button->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
}

void NavigationSidebar::Refresh()
{
    for (int index = 0; index < static_cast<int>(buttons.size()); ++index)
    {
        auto* button = buttons[static_cast<std::size_t>(index)];
        button->setText(pages->tabText(index));
        button->setToolTip(pages->tabText(index));
        button->setAccessibleName(pages->tabText(index));
        button->setChecked(pages->currentIndex() == index);
    }
}

bool NavigationSidebar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == pages && event->type() == QEvent::EnabledChange)
        setEnabled(pages->isEnabled());
    return QWidget::eventFilter(watched, event);
}
