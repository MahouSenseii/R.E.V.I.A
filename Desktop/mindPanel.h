#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

class QLabel;
class QTableWidget;
class QTabWidget;

// Developer visibility into the state that decides how Revia behaves.
//
// Emotion, mood, development, and relationships all change slowly and invisibly. Without
// somewhere to look at them, "she seems more cautious lately" is unfalsifiable and a
// broken appraisal is indistinguishable from a quiet one. This is the panel that makes
// the difference checkable.
//
// Read-only, and deliberately so. Every value here is earned from recorded evidence;
// a control that let a developer set trust to 0.9 would be exactly the assignment path
// the relationship system refuses the language model.
//
// This is a debug surface. Revia is not asked to recite these numbers in conversation.
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
