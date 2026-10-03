#pragma once

#include "Identity/relationshipState.h"
#include "Identity/socialIdentity.h"

#include <QWidget>
#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;

class AudienceStudioPanel final : public QWidget
{
  public:
    struct Controls
    {
        std::function<void(revia::identity::AudienceContext)> audience;
        std::function<void(const std::string&, const std::string&, bool)> enroll;
        std::function<void(const std::string&)> forget;
        std::function<void(const std::string&, const std::string&)> alias;
        std::function<void(const std::string&, const std::string&)> correct;
    };
    explicit AudienceStudioPanel(Controls controls, QWidget* parent = nullptr);
    void SetSnapshot(const revia::identity::AudienceContext& audience, const std::vector<revia::identity::RelationshipState>& people,
        const std::vector<revia::identity::RelationshipEvidenceRecord>& evidence);
    void SetBusy(bool busy);
    void SetOutcome(bool success, const std::string& message);
    void ResetForCompanion();

  private:
    void ShowEvidence();
    Controls controls;
    bool busy = false;
    QLabel* summary = nullptr;
    QLabel* outcome = nullptr;
    QPlainTextEdit* evidenceDetails = nullptr;
    std::vector<revia::identity::RelationshipEvidenceRecord> retainedEvidence;
    QComboBox* kind = nullptr;
    QComboBox* people = nullptr;
    QComboBox* recipients = nullptr;
    QComboBox* evidence = nullptr;
    QLineEdit* audienceId = nullptr;
    QLineEdit* wave = nullptr;
    QLineEdit* alias = nullptr;
    QCheckBox* consent = nullptr;
    QPushButton* enroll = nullptr;
    std::vector<QPushButton*> operations;
    std::uint64_t observedRevision = 0;
};
