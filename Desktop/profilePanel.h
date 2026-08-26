#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// Who Revia is, and which created voice says it.
//
// Profiles are edited here and voices are created in the Voice tab, because the two are
// different lifetimes: a voice is built once and reused, while a profile picks one. The
// assignment therefore belongs to the profile, not to the voice studio that made it.
//
// The panel edits identity only. Nothing here can reach a capability, a budget, or a
// policy; those stay behind CapabilityEditor under Permissions.
class ProfilePanel final : public QWidget
{
public:
    explicit ProfilePanel(
        revia::runtime::ReviaSession& session,
        QWidget* parent = nullptr);

    // Re-reads profiles, the active profile, and the created-voice list. Called after
    // startup and whenever the voice studio creates a preset.
    void Refresh();

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
    [[nodiscard]] const revia::runtime::ProfileSummary* FindProfile(
        const std::string& profileId) const;

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
