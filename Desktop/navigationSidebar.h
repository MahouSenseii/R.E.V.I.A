#pragma once

#include <QWidget>

#include <functional>
#include <vector>

class QLabel;
class QTabWidget;
class QToolButton;

class NavigationSidebar final : public QWidget
{
  public:
    NavigationSidebar(QTabWidget* pages, std::function<void()> layoutChanged, QWidget* parent = nullptr);
    void SetNarrow(bool narrow);
    void Refresh();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void ApplyMode();

    QTabWidget* pages;
    std::function<void()> layoutChanged;
    QLabel* brand = nullptr;
    QLabel* heading = nullptr;
    QToolButton* toggle = nullptr;
    std::vector<QToolButton*> buttons;
    bool narrow = false;
    bool userCollapsed = false;
};
