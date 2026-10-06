#pragma once

#include "Agents/agentWorkflow.h"
#include "Runtime/agentProviderMode.h"

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include <functional>
#include <string>

class QLabel;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

class AgentStudioPanel final : public QWidget
{
  public:
    struct Controls
    {
        std::function<bool(const std::string&, revia::runtime::AgentProviderMode, std::string&)> start;
        std::function<void()> cancel;
        std::function<bool(std::string&)> resume;
        std::function<bool(const std::string&, const std::string&, const std::string&, std::string&)> retry;
        std::function<bool(revia::agents::ParentDecision, std::string&)> decide;
        std::function<std::string()> deliverable;
    };

    explicit AgentStudioPanel(Controls controls, QWidget* parent = nullptr);
    void SetSnapshot(const revia::agents::WorkflowSnapshot& snapshot);
    void ResetForCompanion();

  private:
    void ShowResult(bool success, const std::string& message);
    void ShowSelectedNode();

    Controls controls;
    revia::agents::WorkflowSnapshot snapshot;
    QElapsedTimer observationAge;
    QString observedUtc;
    QLabel* summary = nullptr;
    QLabel* feedback = nullptr;
    QLabel* details = nullptr;
    QComboBox* provider = nullptr;
    QComboBox* retryNode = nullptr;
    QPlainTextEdit* objective = nullptr;
    QPlainTextEdit* revisedInput = nullptr;
    QPlainTextEdit* evidence = nullptr;
    QPlainTextEdit* deliverable = nullptr;
    QTreeWidget* hierarchy = nullptr;
    QPushButton* start = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* accept = nullptr;
};
