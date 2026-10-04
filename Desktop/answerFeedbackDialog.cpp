#include "answerFeedbackDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>

AnswerFeedbackDialog::AnswerFeedbackDialog(revia::learning::QualityFeedback value, const QString& displayedReply, QWidget* parent)
    : QDialog(parent), target(std::move(value))
{
    setObjectName("answerFeedbackDialog");
    setWindowTitle("Review displayed answer");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(540, 480);
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel("Judge this displayed reply against one explicit criterion. Your feedback creates a private "
                                   "candidate for Skills & Learning review; it does not accept a lesson.",
        this);
    explanation->setWordWrap(true);
    explanation->setTextFormat(Qt::PlainText);
    layout->addWidget(explanation);
    auto* reply = new QPlainTextEdit(this);
    reply->setObjectName("answerFeedbackTarget");
    reply->setReadOnly(true);
    reply->setPlainText(displayedReply);
    reply->setMaximumHeight(145);
    layout->addWidget(reply);
    issue = new QComboBox(this);
    issue->setObjectName("answerFeedbackIssue");
    for (const auto value : {revia::learning::QualityIssue::AnswerCoverage, revia::learning::QualityIssue::UnsupportedClaim,
             revia::learning::QualityIssue::MemoryContinuity, revia::learning::QualityIssue::MissingAcceptanceEvidence})
        issue->addItem(QString::fromStdString(revia::learning::ToString(value)), static_cast<int>(value));
    layout->addWidget(issue);
    criterion = new QLineEdit(this);
    criterion->setObjectName("answerFeedbackCriterion");
    criterion->setMaxLength(1000);
    criterion->setPlaceholderText("What did this answer need to satisfy?");
    layout->addWidget(criterion);
    evidence = new QPlainTextEdit(this);
    evidence->setObjectName("answerFeedbackEvidence");
    evidence->setPlaceholderText("What in the displayed reply failed that criterion?");
    evidence->setMaximumHeight(100);
    layout->addWidget(evidence);
    outcome = new QLabel(this);
    outcome->setObjectName("answerFeedbackOutcome");
    outcome->setWordWrap(true);
    outcome->setTextFormat(Qt::PlainText);
    outcome->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(outcome);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    record = buttons->addButton("Record judged issue", QDialogButtonBox::ActionRole);
    record->setObjectName("answerFeedbackRecord");
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

bool AnswerFeedbackDialog::CaptureFeedback(revia::learning::QualityFeedback& outFeedback, std::string& outError) const
{
    outFeedback = target;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    outFeedback.judgmentId = id;
    outFeedback.sourceId = "owner-review:" + id;
    outFeedback.issue = static_cast<revia::learning::QualityIssue>(issue->currentData().toInt());
    outFeedback.criterion = criterion->text().trimmed().toStdString();
    outFeedback.evidence = evidence->toPlainText().trimmed().toStdString();
    return revia::learning::ValidateQualityFeedback(outFeedback, outError);
}

void AnswerFeedbackDialog::SetBusy(const bool busy)
{
    record->setEnabled(!busy);
    issue->setEnabled(!busy);
    criterion->setEnabled(!busy);
    evidence->setEnabled(!busy);
}

void AnswerFeedbackDialog::SetOutcome(const bool success, const std::string& message)
{
    outcome->setText(QString::fromStdString(message));
    outcome->setProperty("error", !success);
}

QPushButton* AnswerFeedbackDialog::RecordButton() const
{
    return record;
}
