#include "chatSessionsPanel.h"

#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
QString SessionLabel(const revia::memory::ArchivedSession& session, const bool current)
{
    QString name = QString::fromStdString(session.title.empty() ? session.opening : session.title).simplified();
    if (name.isEmpty())
        name = current ? QStringLiteral("New chat") : QStringLiteral("Untitled chat");
    if (name.size() > 60)
        name = name.left(57) + QStringLiteral("...");
    bool parsed = false;
    const qint64 started = QString::fromStdString(session.startedAt).toLongLong(&parsed);
    QString detail = parsed && started > 0 ? QDateTime::fromSecsSinceEpoch(started).toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
    detail += (detail.isEmpty() ? QString() : QStringLiteral("  ·  ")) + QString::number(session.turns) +
              (session.turns == 1 ? QStringLiteral(" turn") : QStringLiteral(" turns"));
    if (current)
        detail += QStringLiteral("  ·  current");
    return name + QLatin1Char('\n') + detail;
}
}

ChatSessionsPanel::ChatSessionsPanel(ChatSessionControls inputControls, QWidget* parent)
    : QWidget(parent), controls(std::move(inputControls))
{
    setObjectName("chatSessionsPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* heading = new QLabel(QStringLiteral("Chats"), this);
    heading->setObjectName("chatSessionsHeading");
    layout->addWidget(heading);
    newChat = new QPushButton(QStringLiteral("New chat"), this);
    newChat->setObjectName("newChatButton");
    newChat->setToolTip(QStringLiteral("Start with an empty conversation. What Revia remembers long term is not touched."));
    layout->addWidget(newChat);
    search = new QLineEdit(this);
    search->setObjectName("chatSearch");
    search->setPlaceholderText(QStringLiteral("Search chats"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    list = new QListWidget(this);
    list->setObjectName("chatSessionList");
    list->setWordWrap(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setToolTip(QStringLiteral("Open a chat to read it. Past chats are read-only unless you continue one."));
    // The list gives up height first, so the panel never makes the chat page taller
    // than the window and pushes the message box out of view.
    list->setMinimumHeight(36);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    layout->addWidget(list, 1);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(6);
    rename = new QPushButton(QStringLiteral("Rename"), this);
    rename->setObjectName("renameChatButton");
    remove = new QPushButton(QStringLiteral("Delete"), this);
    remove->setObjectName("deleteChatButton");
    remove->setToolTip(QStringLiteral("Delete this chat from the archive. Long-term memory is not touched."));
    exportChat = new QPushButton(QStringLiteral("Export"), this);
    exportChat->setObjectName("exportChatButton");
    exportChat->setToolTip(QStringLiteral("Save this chat to a file you choose"));
    auto* exportMenu = new QMenu(exportChat);
    connect(exportMenu->addAction(QStringLiteral("Markdown (.md)")), &QAction::triggered, this, [this]() { ChooseExport(false); });
    connect(exportMenu->addAction(QStringLiteral("JSON (.json)")), &QAction::triggered, this, [this]() { ChooseExport(true); });
    exportChat->setMenu(exportMenu);
    actions->addWidget(rename);
    actions->addWidget(remove);
    actions->addWidget(exportChat);
    layout->addLayout(actions);

    status = new QLabel(this);
    status->setObjectName("chatSessionsStatus");
    status->setWordWrap(true);
    status->hide();
    layout->addWidget(status);

    // Searching on every keystroke would query the archive for each letter.
    searchDelay = new QTimer(this);
    searchDelay->setSingleShot(true);
    searchDelay->setInterval(250);
    connect(searchDelay, &QTimer::timeout, this, [this]() { Refresh(); });
    connect(search, &QLineEdit::textChanged, this, [this]() { searchDelay->start(); });
    connect(list, &QListWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
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
    connect(newChat, &QPushButton::clicked, this,
        [this]()
        {
            std::string error;
            if (!controls.startNew || !controls.startNew(error))
            {
                SetStatus(QString::fromStdString(error.empty() ? std::string("A new chat could not be started.") : error));
                return;
            }
            search->clear();
            Refresh();
            SetStatus(QString::fromStdString(error));
        });
    connect(rename, &QPushButton::clicked, this, [this]() { RenameSelected(); });
    connect(remove, &QPushButton::clicked, this, [this]() { DeleteSelected(); });
    UpdateButtons();
}

void ChatSessionsPanel::Refresh()
{
    const std::string selected = SelectedId();
    std::string error;
    const std::vector<revia::memory::ArchivedSession> sessions =
        controls.list ? controls.list(search->text().trimmed().toStdString(), error) : std::vector<revia::memory::ArchivedSession>{};
    const std::string current = controls.current ? controls.current() : std::string();
    list->clear();
    for (const auto& session : sessions)
    {
        auto* item = new QListWidgetItem(SessionLabel(session, session.id == current), list);
        item->setData(Qt::UserRole, QString::fromStdString(session.id));
        item->setToolTip(QString::fromStdString(session.opening).left(300));
        if (session.id == selected)
            item->setSelected(true);
    }
    if (!error.empty())
        SetStatus(QString::fromStdString(error));
    else if (sessions.empty() && !search->text().trimmed().isEmpty())
        SetStatus(QStringLiteral("No chat matches that search."));
    else
        SetStatus(QString());
    newChat->setEnabled(controls.startNew != nullptr);
    UpdateButtons();
}

std::string ChatSessionsPanel::SelectedId() const
{
    const auto selected = list->selectedItems();
    return selected.isEmpty() ? std::string() : selected.front()->data(Qt::UserRole).toString().toStdString();
}

void ChatSessionsPanel::SetStatus(const QString& text)
{
    status->setText(text);
    status->setVisible(!text.isEmpty());
}

void ChatSessionsPanel::UpdateButtons()
{
    const bool chosen = !SelectedId().empty();
    for (QPushButton* button : {rename, remove, exportChat})
        button->setEnabled(chosen);
}

void ChatSessionsPanel::RenameSelected()
{
    const std::string id = SelectedId();
    if (id.empty() || !controls.rename)
        return;
    bool accepted = false;
    const QString title = QInputDialog::getText(this, QStringLiteral("Rename chat"), QStringLiteral("Name"), QLineEdit::Normal,
        list->selectedItems().front()->text().section(QLatin1Char('\n'), 0, 0), &accepted);
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

void ChatSessionsPanel::DeleteSelected()
{
    const std::string id = SelectedId();
    if (id.empty() || !controls.remove)
        return;
    const bool current = controls.current && controls.current() == id;
    const QString question = current ? QStringLiteral("Delete the chat in progress? Its turns leave the archive and Revia's working "
                                                      "context. What she remembers long term is not touched.")
                                     : QStringLiteral("Delete this chat from the archive? What Revia remembers long term is not touched.");
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
    SetStatus(QString::fromStdString(error));
}

void ChatSessionsPanel::ChooseExport(const bool json)
{
    if (SelectedId().empty())
        return;
    const QString suffix = json ? QStringLiteral("json") : QStringLiteral("md");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export chat"),
        QDir::home().filePath(QStringLiteral("revia-chat.") + suffix),
        json ? QStringLiteral("JSON (*.json)") : QStringLiteral("Markdown (*.md)"));
    if (path.isEmpty())
        return;
    QString error;
    SetStatus(ExportSelected(path, json, error) ? QStringLiteral("Exported to ") + QDir::toNativeSeparators(path) : error);
}

bool ChatSessionsPanel::ExportSelected(const QString& path, const bool json, QString& error)
{
    const std::string id = SelectedId();
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
