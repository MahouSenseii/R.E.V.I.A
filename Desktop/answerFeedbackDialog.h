#pragma once

#include "Learning/qualityFeedback.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class AnswerFeedbackDialog final : public QDialog
{
  public:
    AnswerFeedbackDialog(revia::learning::QualityFeedback target, const QString& displayedReply, QWidget* parent = nullptr);
    bool CaptureFeedback(revia::learning::QualityFeedback& outFeedback, std::string& outError) const;
    // The owner asked to keep this reply's exact exchange for a training dataset.
    bool KeepExchange() const;
    void SetBusy(bool busy);
    void SetOutcome(bool success, const std::string& message);
    QPushButton* RecordButton() const;

  private:
    revia::learning::QualityFeedback target;
    QComboBox* issue = nullptr;
    QLineEdit* criterion = nullptr;
    QPlainTextEdit* evidence = nullptr;
    QCheckBox* keepExchange = nullptr;
    QLabel* outcome = nullptr;
    QPushButton* record = nullptr;
};
