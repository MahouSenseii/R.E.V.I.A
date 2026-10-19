#pragma once

#include <QDialog>
#include <QString>

#include <utility>
#include <vector>

namespace revia::desktop
{
// Ctrl+/: every keyboard shortcut in a two-column table. Each row is {action, keys}, for example
// {"New chat", "Ctrl+N"}; a row with empty keys is a section heading ({"Panel windows", ""}).
// Modeless, owned by the window, built once and shown again on demand.
class ShortcutSheet final : public QDialog
{
  public:
    ShortcutSheet(std::vector<std::pair<QString, QString>> rows, QWidget* parent = nullptr);
};
}
