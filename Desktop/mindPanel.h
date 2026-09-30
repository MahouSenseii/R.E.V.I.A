#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

class QLabel;
class QTableWidget;
class QTabWidget;

// Read-only diagnostics for evidence-derived emotion, mood, development and relationships.
// These values are not conversation output or developer-editable state.
class MindPanel final : public QWidget
{
public:
    explicit MindPanel(revia::runtime::ReviaSession& session, QWidget* parent = nullptr);

    // Cheap enough for the shell's poll timer: it reads snapshots the session already
    // keeps and touches no model, no disk, and no lock the runtime holds for long.
    void Refresh();

private:
    void RenderEmotion();
    void RenderDevelopment();
    void RenderRelationships();
    void RenderDrives();

    revia::runtime::ReviaSession& session;

    QTabWidget* views = nullptr;

    QLabel* dominantLabel = nullptr;
    QLabel* moodLabel = nullptr;
    QTableWidget* emotionTable = nullptr;

    QLabel* driftLabel = nullptr;
    QTableWidget* developmentTable = nullptr;
    QTableWidget* historyTable = nullptr;

    QLabel* relationshipSummary = nullptr;
    QTableWidget* relationshipTable = nullptr;

    QLabel* activityLabel = nullptr;
    QLabel* decisionLabel = nullptr;
    QTableWidget* driveTable = nullptr;
};
