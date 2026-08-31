#include "mindPanel.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QHeaderView>
#include <QLabel>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>

namespace
{
    using revia::emotion::Emotion;
    using revia::emotion::EmotionCount;
    using revia::identity::Trait;
    using revia::identity::TraitCount;

    QString Capitalised(std::string value)
    {
        if (!value.empty())
        {
            value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
        }
        return QString::fromStdString(value);
    }

    QString Fixed(const float value, const int decimals = 2)
    {
        return QString::number(static_cast<double>(value), 'f', decimals);
    }

    QTableWidget* MakeTable(QWidget* parent, const QStringList& headers)
    {
        auto* table = new QTableWidget(parent);
        table->setColumnCount(headers.size());
        table->setHorizontalHeaderLabels(headers);
        table->verticalHeader()->setVisible(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setWordWrap(false);
        table->setTextElideMode(Qt::ElideRight);
        table->verticalHeader()->setDefaultSectionSize(26);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < headers.size(); ++column)
        {
            table->horizontalHeader()->setSectionResizeMode(
                column, QHeaderView::ResizeToContents);
        }
        return table;
    }

    void SetCell(QTableWidget* table, const int row, const int column, const QString& text)
    {
        table->setItem(row, column, new QTableWidgetItem(text));
    }
}

MindPanel::MindPanel(revia::runtime::ReviaSession& inputSession, QWidget* parent)
    : QWidget(parent),
      session(inputSession)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* title = new QLabel("Mind", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);

    auto* explanation = new QLabel(
        "The state that decides how Revia behaves, as the runtime actually holds it. "
        "Emotions coexist rather than replacing one another, mood moves far more slowly "
        "than emotion, and personality and relationships change only from accumulated "
        "evidence. Read-only: everything here is earned, not set.", this);
    explanation->setWordWrap(true);
    explanation->setObjectName("secondaryText");
    layout->addWidget(explanation);

    views = new QTabWidget(this);
    views->setDocumentMode(true);

    // --- Now ----------------------------------------------------------------------
    auto* now = new QWidget(views);
    auto* nowLayout = new QVBoxLayout(now);
    nowLayout->setContentsMargins(0, 8, 0, 0);
    dominantLabel = new QLabel(now);
    dominantLabel->setObjectName("mindHeadline");
    dominantLabel->setWordWrap(true);
    nowLayout->addWidget(dominantLabel);
    moodLabel = new QLabel(now);
    moodLabel->setObjectName("secondaryText");
    moodLabel->setWordWrap(true);
    nowLayout->addWidget(moodLabel);
    emotionTable = MakeTable(now, {"Emotion", "Value"});
    nowLayout->addWidget(emotionTable, 1);
    views->addTab(now, "Now");

    // --- Development --------------------------------------------------------------
    auto* development = new QWidget(views);
    auto* developmentLayout = new QVBoxLayout(development);
    developmentLayout->setContentsMargins(0, 8, 0, 0);
    driftLabel = new QLabel(development);
    driftLabel->setObjectName("mindHeadline");
    driftLabel->setWordWrap(true);
    developmentLayout->addWidget(driftLabel);
    // Base and current shown side by side on purpose. The delta between them is the
    // whole record of who she has become, and collapsing them would throw it away.
    developmentTable = MakeTable(development, {"Trait", "Started", "Change", "Now"});
    developmentLayout->addWidget(developmentTable, 2);
    auto* historyTitle = new QLabel("Applied changes, most recent last", development);
    historyTitle->setObjectName("secondaryText");
    developmentLayout->addWidget(historyTitle);
    historyTable = MakeTable(development, {"Why", "Trait", "Change", "Evidence", "When"});
    developmentLayout->addWidget(historyTable, 1);
    views->addTab(development, "Development");

    // --- Relationships ------------------------------------------------------------
    auto* people = new QWidget(views);
    auto* peopleLayout = new QVBoxLayout(people);
    peopleLayout->setContentsMargins(0, 8, 0, 0);
    relationshipSummary = new QLabel(people);
    relationshipSummary->setObjectName("mindHeadline");
    relationshipSummary->setWordWrap(true);
    peopleLayout->addWidget(relationshipSummary);
    relationshipTable = MakeTable(people, {
        "Who", "Familiar", "Affinity", "Trust", "Respect", "Irritation", "Resentment",
        "Exchanges"});
    peopleLayout->addWidget(relationshipTable, 1);
    views->addTab(people, "Relationships");

    // --- Drives ---------------------------------------------------------------------
    auto* wanting = new QWidget(views);
    auto* wantingLayout = new QVBoxLayout(wanting);
    wantingLayout->setContentsMargins(0, 8, 0, 0);
    decisionLabel = new QLabel(wanting);
    decisionLabel->setObjectName("mindHeadline");
    decisionLabel->setWordWrap(true);
    wantingLayout->addWidget(decisionLabel);
    activityLabel = new QLabel(wanting);
    activityLabel->setObjectName("secondaryText");
    activityLabel->setWordWrap(true);
    wantingLayout->addWidget(activityLabel);
    driveTable = MakeTable(wanting, {"Drive", "Strength"});
    wantingLayout->addWidget(driveTable, 1);
    views->addTab(wanting, "Drives");

    layout->addWidget(views, 1);
    Refresh();
}

void MindPanel::Refresh()
{
    RenderEmotion();
    RenderDevelopment();
    RenderRelationships();
    RenderDrives();
}

void MindPanel::RenderEmotion()
{
    const revia::emotion::EmotionVector emotion = session.CurrentEmotion();
    const revia::emotion::MoodState mood = session.CurrentMood();
    const revia::emotion::EmotionReading dominant = emotion.Dominant();

    if (dominant.value < 0.12F)
    {
        // Calm is a real state, not a missing reading. Naming the strongest of several
        // negligible flickers would make this panel look busy while nothing is happening.
        dominantLabel->setText("Calm \xE2\x80\x94 nothing in particular is pulling at her.");
    }
    else
    {
        dominantLabel->setText(
            Capitalised(revia::emotion::ToString(dominant.emotion)) + " at " +
            QString::number(static_cast<int>(dominant.value * 100.0F)) + "%");
    }

    moodLabel->setText(
        "Mood \xE2\x80\x94 valence " + Fixed(mood.valence) +
        ", energy " + Fixed(mood.energy) +
        ", irritability " + Fixed(mood.irritability) +
        ", sociability " + Fixed(mood.sociability) +
        "  (mood is the slow layer; it lags emotion deliberately)");

    // Every component, including the zeroes. The point of the vector is that emotions
    // coexist, and hiding the quiet ones would make it look like a single-state enum
    // again.
    emotionTable->setRowCount(static_cast<int>(EmotionCount));
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        const auto value = static_cast<Emotion>(index);
        const float strength = emotion[value];
        SetCell(emotionTable, static_cast<int>(index), 0,
            Capitalised(revia::emotion::ToString(value)));
        auto* reading = new QTableWidgetItem(Fixed(strength));
        if (strength < 0.01F)
        {
            // Dimmed rather than hidden, so "felt nothing" stays visibly different from
            // "not tracked".
            reading->setForeground(QBrush(QColor(0x5a, 0x68, 0x7d)));
        }
        emotionTable->setItem(static_cast<int>(index), 1, reading);
    }
}

void MindPanel::RenderDevelopment()
{
    const revia::identity::DevelopmentState development = session.CurrentDevelopment();
    const std::string drift = development.DescribeDrift();
    driftLabel->setText(drift.empty()
        ? "She is still who she started out as. Nothing has accumulated enough evidence "
          "to move yet."
        : "She is now " + QString::fromStdString(drift) + ".");

    const revia::identity::TraitVector current = development.Current();
    developmentTable->setRowCount(static_cast<int>(TraitCount));
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        const auto trait = static_cast<Trait>(index);
        const float change = development.delta[trait];
        const int row = static_cast<int>(index);
        SetCell(developmentTable, row, 0,
            Capitalised(revia::identity::ToString(trait)));
        SetCell(developmentTable, row, 1, Fixed(development.base[trait]));
        auto* delta = new QTableWidgetItem(
            (change > 0.0F ? "+" : "") + Fixed(change));
        if (std::abs(change) < 0.005F)
        {
            delta->setForeground(QBrush(QColor(0x5a, 0x68, 0x7d)));
        }
        developmentTable->setItem(row, 2, delta);
        SetCell(developmentTable, row, 3, Fixed(current[trait]));
    }

    const std::vector<revia::identity::DevelopmentChange> history =
        session.DevelopmentHistory();
    historyTable->setRowCount(static_cast<int>(history.size()));
    for (std::size_t index = 0; index < history.size(); ++index)
    {
        const revia::identity::DevelopmentChange& change = history[index];
        const int row = static_cast<int>(index);
        auto* reason = new QTableWidgetItem(QString::fromStdString(change.reason));
        reason->setToolTip(QString::fromStdString(change.reason));
        historyTable->setItem(row, 0, reason);
        SetCell(historyTable, row, 1,
            Capitalised(revia::identity::ToString(change.trait)));
        SetCell(historyTable, row, 2,
            (change.delta > 0.0F ? "+" : "") + Fixed(change.delta, 3));
        SetCell(historyTable, row, 3, QString::number(
            static_cast<qulonglong>(change.evidenceCount)));
        SetCell(historyTable, row, 4, QString::fromStdString(change.recordedAt));
    }
}

void MindPanel::RenderRelationships()
{
    const std::vector<revia::identity::RelationshipState> people =
        session.Relationships();
    relationshipSummary->setText(people.empty()
        ? "Nobody yet. A relationship begins on first contact and starts neutral \xE2\x80\x94 "
          "being met is not the same as being liked."
        : QString::number(static_cast<int>(people.size())) +
            (people.size() == 1 ? " person known." : " people known.") +
            "  Warmth is capped by familiarity; dislike is not.");

    relationshipTable->setRowCount(static_cast<int>(people.size()));
    for (std::size_t index = 0; index < people.size(); ++index)
    {
        const revia::identity::RelationshipState& person = people[index];
        const int row = static_cast<int>(index);
        // The name leads. An id is what the runtime needs; a person reading this wants
        // to know who it is, and "local:user" reads as though nothing was ever learned.
        const QString who = person.displayName.empty()
            ? (person.entityId == "local:user"
                ? QString("Unnamed local user  (say \"my name is ...\" to be recognised)")
                : QString::fromStdString(person.entityId))
            : QString::fromStdString(person.displayName) + "  â  " +
                QString::fromStdString(person.entityId);
        auto* label = new QTableWidgetItem(who);
        label->setToolTip(QString::fromStdString(person.DescribeForPrompt()));
        relationshipTable->setItem(row, 0, label);
        SetCell(relationshipTable, row, 1, Fixed(person.familiarity));
        SetCell(relationshipTable, row, 2,
            (person.affinity > 0.0F ? "+" : "") + Fixed(person.affinity));
        SetCell(relationshipTable, row, 3, Fixed(person.trust));
        SetCell(relationshipTable, row, 4, Fixed(person.respect));
        SetCell(relationshipTable, row, 5, Fixed(person.irritation));
        SetCell(relationshipTable, row, 6, Fixed(person.resentment));
        SetCell(relationshipTable, row, 7, QString::number(
            static_cast<qulonglong>(person.interactionCount)));
    }
}

void MindPanel::RenderDrives()
{
    const revia::autonomy::DriveState drives = session.Drives();
    const revia::autonomy::ActivityDecision decision = session.LastAutonomyDecision();

    // The refusal matters as much as the decision. A companion that stays quiet without
    // being able to say why is indistinguishable from one that is broken.
    if (decision.type == revia::autonomy::ActivityType::Nothing)
    {
        decisionLabel->setText(decision.refusal.empty()
            ? "Nothing considered yet."
            : "Doing nothing â " + QString::fromStdString(decision.refusal));
    }
    else
    {
        decisionLabel->setText(
            QString::fromStdString(revia::autonomy::ToString(decision.type)) +
            " â " + QString::fromStdString(decision.reason) +
            "  (score " + Fixed(decision.score) + ")");
    }

    const std::optional<revia::autonomy::Activity> activity = session.CurrentActivity();
    activityLabel->setText(activity
        ? "Last activity: " +
            QString::fromStdString(revia::autonomy::ToString(activity->type)) + " â " +
            QString::fromStdString(revia::autonomy::ToString(activity->status))
        : QString("No activity has been started. Only a real event can start one; time "
                  "passing cannot."));

    driveTable->setRowCount(static_cast<int>(revia::autonomy::DriveCount));
    for (std::size_t index = 0; index < revia::autonomy::DriveCount; ++index)
    {
        const auto drive = static_cast<revia::autonomy::Drive>(index);
        const float strength = drives[drive];
        SetCell(driveTable, static_cast<int>(index), 0,
            Capitalised(revia::autonomy::ToString(drive)));
        auto* reading = new QTableWidgetItem(Fixed(strength));
        if (strength < 0.01F)
        {
            reading->setForeground(QBrush(QColor(0x5a, 0x68, 0x7d)));
        }
        driveTable->setItem(static_cast<int>(index), 1, reading);
    }
}
