#pragma once

#include "Runtime/runtimeEvents.h"

#include <QWidget>

#include <cstdint>
#include <string>
#include <unordered_map>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

// Read-only presentation of the bounded internet lane. It consumes runtime events and
// owns no permissions, networking, or audit state.
class InternetActivityPanel final : public QWidget
{
public:
    explicit InternetActivityPanel(QWidget* parent = nullptr);

    void Observe(const revia::runtime::RuntimeEvent& event);

private:
    int EnsureRow(const revia::runtime::RuntimeEvent& event);
    void RebuildRowIndex();
    void ShowSelectedDetail();

    QTableWidget* table = nullptr;
    QPlainTextEdit* detail = nullptr;
    QPushButton* clearButton = nullptr;
    std::unordered_map<std::string, int> rows;
};
