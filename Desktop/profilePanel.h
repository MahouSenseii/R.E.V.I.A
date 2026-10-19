#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QResizeEvent;
class QPushButton;
class QSpinBox;

// Edits profiles and their reusable voice assignments.
// Capability, budget and policy changes remain in Permissions.
class ProfilePanel final : public QWidget
{
public:
    explicit ProfilePanel(revia::runtime::ReviaSession& session, QWidget* parent = nullptr);

    // Re-reads profiles, the active profile, and the created-voice list. Called after
    // startup and whenever the voice studio creates a preset.
    void Refresh();
    // True while the form holds edits the selected profile does not have on disk, including a new
    // profile that was never saved. Settings refuses its own profile switches meanwhile, because
    // the Refresh that follows such a write would throw these edits away.
    [[nodiscard]] bool HasUnsavedChanges() const;

protected:
    void resizeEvent(QResizeEvent* event) override;
    // A click anywhere on a switch row toggles its switch.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void LoadSelectedProfile();
    void BeginNewProfile();
    void Save();
    void Activate();
    void AssignVoice(bool useWindowsFallback);
    void SetStatus(const QString& text, bool error = false);
    void UpdateBanner();
    // A profile that only exists in the form has no voice assignment to edit and no
    // identity to switch to, so those controls wait until it has been saved once.
    void ApplyDraftState();
    [[nodiscard]] const revia::runtime::ProfileSummary* FindProfile(const std::string& profileId) const;

    revia::runtime::ReviaSession& session;
    revia::runtime::ProfileStudioSnapshot snapshot;

    QLabel* activeBanner = nullptr;
    QComboBox* profileCombo = nullptr;
    QPushButton* activateButton = nullptr;
    QPushButton* newProfileButton = nullptr;
    QLineEdit* idInput = nullptr;
    QLineEdit* displayNameInput = nullptr;
    QLineEdit* descriptionInput = nullptr;
    QPlainTextEdit* systemPromptInput = nullptr;
    QCheckBox* memoryCheck = nullptr;
    QComboBox* answerStyleCombo = nullptr;
    QCheckBox* temperatureCheck = nullptr;
    QDoubleSpinBox* temperatureSpin = nullptr;
    QCheckBox* maxTokensCheck = nullptr;
    QSpinBox* maxTokensSpin = nullptr;
    QComboBox* voiceCombo = nullptr;
    QPushButton* assignVoiceButton = nullptr;
    QPushButton* windowsVoiceButton = nullptr;
    QPushButton* saveButton = nullptr;
    QPushButton* revertButton = nullptr;
    QLabel* statusLabel = nullptr;

    // True while the form holds a profile that has never been written to disk.
    bool draftProfile = false;
    // Guards the combo's change handler while Refresh rebuilds it.
    bool refreshing = false;
};
