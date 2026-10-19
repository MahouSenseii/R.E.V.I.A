#include "chatSessionsPanel.h"

#include "theme.h"

#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
// The line a chat is listed under: its title, else its opening line.
QString SessionLabel(const revia::memory::ArchivedSession& session, const bool current)
{
    QString name = QString::fromStdString(session.title.empty() ? session.opening : session.title).simplified();
    if (name.isEmpty())
        name = current ? QStringLiteral("New chat") : QStringLiteral("Untitled chat");
    if (name.size() > 60)
        name = name.left(57) + QStringLiteral("...");
    return name;
}

// The date and length moved off the row into its tooltip, with the opening line.
QString SessionDetail(const revia::memory::ArchivedSession& session, const bool current)
{
    bool parsed = false;
    const qint64 started = QString::fromStdString(session.startedAt).toLongLong(&parsed);
    QString detail = parsed && started > 0 ? QDateTime::fromSecsSinceEpoch(started).toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
    detail += (detail.isEmpty() ? QString() : QStringLiteral("  ·  ")) + QString::number(session.turns) +
              (session.turns == 1 ? QStringLiteral(" turn") : QStringLiteral(" turns"));
    if (current)
        detail += QStringLiteral("  ·  current");
    const QString opening = QString::fromStdString(session.opening).left(300);
    return opening.isEmpty() ? detail : detail + QLatin1Char('\n') + opening;
}
}

ChatSessionsPanel::ChatSessionsPanel(ChatSessionControls inputControls, QWidget* parent)
    : QWidget(parent), controls(std::move(inputControls))
{
    setObjectName("chatSessionsPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    newChat = new QPushButton(QStringLiteral("New chat"), this);
    newChat->setObjectName("newChatButton");
    newChat->setFlat(true);
    newChat->setFocusPolicy(Qt::TabFocus);
    newChat->setToolTip(QStringLiteral("New chat (Ctrl+N)"));
    newChat->setAccessibleName(QStringLiteral("New chat"));
    revia::desktop::theme::BindIcon(newChat, "plus", 16);
    layout->addWidget(newChat);
    search = new QLineEdit(this);
    search->setObjectName("chatSearch");
    search->setPlaceholderText(QStringLiteral("Search chats"));
    search->setAccessibleName(QStringLiteral("Search chats"));
    search->setClearButtonEnabled(true);
    revia::desktop::theme::BindIcon(search->addAction(QIcon(), QLineEdit::LeadingPosition), "search");
    layout->addWidget(search);
    layout->addSpacing(8);

    // The old heading stays as an object for anything that looks it up, but the folding
    // "Recents" label takes its place.
    auto* heading = new QLabel(QStringLiteral("Chats"), this);
    heading->setObjectName("chatSessionsHeading");
    heading->hide();
    listToggle = new QPushButton(QStringLiteral("Recents"), this);
    listToggle->setObjectName("chatSidebarToggle");
    listToggle->setCheckable(true);
    listToggle->setChecked(true);
    listToggle->setFlat(true);
    listToggle->setFocusPolicy(Qt::TabFocus);
    listToggle->setToolTip(QStringLiteral("Show or hide recent chats"));
    revia::desktop::theme::BindIcon(listToggle, "chevron-down", 14);
    layout->addWidget(listToggle, 0, Qt::AlignLeft);

    // Under the heading it explains, not below an empty list at the foot of the sidebar.
    status = new QLabel(this);
    status->setObjectName("chatSessionsStatus");
    status->setWordWrap(true);
    status->hide();
    layout->addWidget(status);

    list = new QListWidget(this);
    list->setObjectName("chatSessionList");
    list->setAccessibleName(QStringLiteral("Recent chats"));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setTextElideMode(Qt::ElideRight);
    list->setUniformItemSizes(true);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    list->setToolTip(QStringLiteral("Open a chat to read it. Past chats are read-only unless you continue one."));
    // The list gives up height first, so the panel never pushes the sidebar footer away.
    list->setMinimumHeight(36);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    layout->addWidget(list, 1);

    // Kept as objects (and for anything that drives them), never shown: the row context menu and
    // the chat header's menu carry these actions now.
    rename = new QPushButton(QStringLiteral("Rename"), this);
    rename->setObjectName("renameChatButton");
    remove = new QPushButton(QStringLiteral("Delete"), this);
    remove->setObjectName("deleteChatButton");
    exportChat = new QPushButton(QStringLiteral("Export"), this);
    exportChat->setObjectName("exportChatButton");
    for (QPushButton* button : {rename, remove, exportChat})
        button->hide();

    // Searching on every keystroke would query the archive for each letter.
    searchDelay = new QTimer(this);
    searchDelay->setSingleShot(true);
    searchDelay->setInterval(250);
    connect(searchDelay, &QTimer::timeout, this, [this]() { Refresh(); });
    connect(search, &QLineEdit::textChanged, this, [this]() { searchDelay->start(); });
    connect(list, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item)
        {
            if (item && controls.open)
                controls.open(item->data(Qt::UserRole).toString().toStdString());
        });
    connect(list, &QListWidget::itemClicked, this,
        [this](QListWidgetItem* item)
        {
            if (item && controls.open)
                controls.open(item->data(Qt::UserRole).toString().toStdString());
        });
    connect(list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& position) { ShowRowMenu(position); });
    connect(newChat, &QPushButton::clicked, this, [this]() { StartNewChat(); });
    connect(listToggle, &QPushButton::toggled, this, [this](const bool shown) { SetListVisible(shown); });
    connect(rename, &QPushButton::clicked, this, [this]() { RenameChat(TargetId()); });
    connect(remove, &QPushButton::clicked, this, [this]() { DeleteChat(TargetId()); });

    auto* renameKey = new QShortcut(QKeySequence(Qt::Key_F2), list);
    renameKey->setContext(Qt::WidgetShortcut);
    connect(renameKey, &QShortcut::activated, this, [this]() { RenameChat(TargetId()); });
    auto* deleteKey = new QShortcut(QKeySequence(QKeySequence::Delete), list);
    deleteKey->setContext(Qt::WidgetShortcut);
    connect(deleteKey, &QShortcut::activated, this, [this]() { DeleteChat(TargetId()); });
}

void ChatSessionsPanel::Refresh()
{
    const std::string selected = SelectedId();
    std::string error;
    sessions = controls.list ? controls.list(search->text().trimmed().toStdString(), error) : std::vector<revia::memory::ArchivedSession>{};
    loadFailed = !error.empty();
    const std::string current = CurrentId();
    list->clear();
    for (const auto& session : sessions)
    {
        const bool isCurrent = session.id == current;
        auto* item = new QListWidgetItem(SessionLabel(session, isCurrent), list);
        item->setData(Qt::UserRole, QString::fromStdString(session.id));
        item->setToolTip(SessionDetail(session, isCurrent));
        if (isCurrent)
        {
            // Marked, not selected: the selection is the owner's (Export acts on it).
            QFont font = item->font();
            font.setWeight(QFont::DemiBold);
            item->setFont(font);
            item->setBackground(revia::desktop::theme::Current().sidebarSelected);
        }
        if (session.id == selected)
            item->setSelected(true);
    }
    if (!error.empty())
        SetStatus(QString::fromStdString(error));
    else if (sessions.empty() && !search->text().trimmed().isEmpty())
        SetStatus(QStringLiteral("No chat matches that search."));
    else if (sessions.empty())
        SetStatus(QStringLiteral("No saved chats yet."));
    else
        SetStatus(QString());
    newChat->setEnabled(controls.startNew != nullptr);
}

std::string ChatSessionsPanel::SelectedId() const
{
    const auto selected = list->selectedItems();
    return selected.isEmpty() ? std::string() : selected.front()->data(Qt::UserRole).toString().toStdString();
}

std::string ChatSessionsPanel::CurrentId() const
{
    return controls.current ? controls.current() : std::string();
}

std::string ChatSessionsPanel::TargetId() const
{
    // The row the owner highlighted (a click also opens it), never the live chat behind it.
    const std::string selected = SelectedId();
    return selected.empty() ? CurrentId() : selected;
}

void ChatSessionsPanel::SetStatus(const QString& text)
{
    status->setText(text);
    status->setVisible(!text.isEmpty());
}

void ChatSessionsPanel::StartNewChat()
{
    std::string error;
    if (!controls.startNew || !controls.startNew(error))
    {
        SetStatus(QString::fromStdString(error.empty() ? std::string("A new chat could not be started.") : error));
        return;
    }
    search->clear();
    Refresh();
    if (!error.empty())
        SetStatus(QString::fromStdString(error));
}

void ChatSessionsPanel::SetListVisible(const bool visible)
{
    list->setVisible(visible);
    if (listToggle->isChecked() != visible)
        listToggle->setChecked(visible);
    revia::desktop::theme::BindIcon(listToggle, visible ? "chevron-down" : "chevron-right", 14);
}

QString ChatSessionsPanel::TitleFor(const std::string& id) const
{
    const auto found = std::find_if(sessions.begin(), sessions.end(), [&id](const auto& session) { return session.id == id; });
    return found == sessions.end() ? QString() : SessionLabel(*found, id == CurrentId());
}

QString ChatSessionsPanel::CurrentTitle() const
{
    const std::string current = CurrentId();
    const QString title = current.empty() ? QString() : TitleFor(current);
    return title.isEmpty() ? QStringLiteral("New chat") : title;
}

void ChatSessionsPanel::ShowRowMenu(const QPoint& position)
{
    QListWidgetItem* item = list->itemAt(position);
    if (item == nullptr)
        return;
    // A right-click acts on the row clicked, whichever chat is current.
    const std::string id = item->data(Qt::UserRole).toString().toStdString();
    QMenu menu(this);
    revia::desktop::theme::PolishMenu(&menu);
    connect(menu.addAction(QStringLiteral("Rename")), &QAction::triggered, this, [this, id]() { RenameChat(id); });
    connect(menu.addAction(QStringLiteral("Export as Markdown")), &QAction::triggered, this, [this, id]() { ExportChat(id, false); });
    connect(menu.addAction(QStringLiteral("Export as JSON")), &QAction::triggered, this, [this, id]() { ExportChat(id, true); });
    menu.addSeparator();
    connect(menu.addAction(QStringLiteral("Delete")), &QAction::triggered, this, [this, id]() { DeleteChat(id); });
    menu.exec(list->viewport()->mapToGlobal(position));
}

void ChatSessionsPanel::RenameChat(const std::string& id)
{
    if (id.empty() || !controls.rename)
        return;
    bool accepted = false;
    const QString title = QInputDialog::getText(this, QStringLiteral("Rename chat"), QStringLiteral("Name"), QLineEdit::Normal, TitleFor(id), &accepted);
    if (!accepted)
        return;
    std::string error;
    if (!controls.rename(id, title.toStdString(), error))
    {
        SetStatus(QString::fromStdString(error));
        return;
    }
    Refresh();
}

void ChatSessionsPanel::DeleteChat(const std::string& id)
{
    if (id.empty() || !controls.remove)
        return;
    const bool current = id == CurrentId();
    const QString question = current ? QStringLiteral("Delete the chat in progress? Its turns leave the archive and the working "
                                                      "context. Long-term memory is not touched.")
                                     : QStringLiteral("Delete this chat from the archive? Long-term memory is not touched.");
    if (QMessageBox::question(this, QStringLiteral("Delete chat"), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes)
        return;
    std::string error;
    if (!controls.remove(id, error))
    {
        SetStatus(QString::fromStdString(error));
        return;
    }
    Refresh();
    if (!error.empty())
        SetStatus(QString::fromStdString(error));
}

void ChatSessionsPanel::ExportChat(const std::string& id, const bool json)
{
    if (id.empty())
        return;
    const QString suffix = json ? QStringLiteral("json") : QStringLiteral("md");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export chat"),
        QDir::home().filePath(QStringLiteral("revia-chat.") + suffix),
        json ? QStringLiteral("JSON (*.json)") : QStringLiteral("Markdown (*.md)"));
    if (path.isEmpty())
        return;
    QString error;
    SetStatus(WriteExport(id, path, json, error) ? QStringLiteral("Exported to ") + QDir::toNativeSeparators(path) : error);
}

bool ChatSessionsPanel::ExportSelected(const QString& path, const bool json, QString& error)
{
    return WriteExport(SelectedId(), path, json, error);
}

bool ChatSessionsPanel::WriteExport(const std::string& id, const QString& path, const bool json, QString& error)
{
    std::string text;
    std::string failure;
    if (id.empty() || !controls.exportText || !controls.exportText(id, json, text, failure))
    {
        error = QString::fromStdString(failure.empty() ? std::string("Choose a chat to export.") : failure);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(text.data(), static_cast<qint64>(text.size())) !=
                                                                         static_cast<qint64>(text.size()))
    {
        error = QStringLiteral("The file could not be written: ") + file.errorString();
        return false;
    }
    return true;
}
