#pragma once

#include "Runtime/reviaSession.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QLabel;
class QComboBox;
class QFrame;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTreeWidget;
class ToggleSwitch;

// Presentation and explicit permission edits only. ReviaSession remains the owner of
// policy persistence/reload and Windows remains the source of discovered identities.
//
// Every switch commits as soon as it changes (risky ones after a confirmation); only the two
// list editors, approved site origins and approved executables, keep a draft with Apply and
// Discard. Switch labels and help text come from the settings catalog.
class CapabilityPanel final : public QWidget
{
public:
    using DiscoveryRequest = std::function<void()>;

    CapabilityPanel(revia::runtime::ReviaSession& session, DiscoveryRequest discoveryRequest, QWidget* parent = nullptr);
    ~CapabilityPanel() override;

    void Refresh();
    void ShowDiscovery(const revia::actions::windows::ApplicationControlInventory& inventory);
    void SetStatus(const QString& text, bool error = false);
    // The internet and interactive-browser cards. They stay laid out in this panel until this is
    // called; afterwards the caller places the returned container (the Settings dialog shows it in
    // its Internet section). The panel still owns it and deletes it with itself.
    QWidget* DetachInternetGroup();
    // Puts both list editors back to the saved lists and drops their unsaved edits.
    void RevertUnsavedLists();
    // Reflows for the width the panel is shown at (design spec 2.11): the permission cards take two
    // columns from 900px, approved applications and discovery sit side by side from 760px, and the
    // tree and table stop insisting on a minimum width.
    void ApplyContentWidth(int width);

protected:
    // Lets a whole permission row act as the switch's hit area. Without it the only
    // target is the switch itself, which is a smaller thing to hit than the checkbox
    // and its label used to be.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void AddApplicationManually();
    void ApproveSelectedDiscoveredControls();
    void RemoveSelectedPermission();
    void ApplyInternetSettings();
    void ApplyBrowserSettings();
    QFrame* CreateInternetCard();
    QFrame* CreateInteractiveBrowserCard();
    QWidget* CreateListFooter(QPushButton*& apply, const QString& applyName, std::function<void()> onApply,
        std::function<void()> onDiscard);
    QFrame* MakeRow(ToggleSwitch* toggle, const QString& tip = QString(), bool nested = false, const QString& riskText = QString());
    void RefreshInteractiveBrowserSettings(const revia::actions::CapabilitySettings& settings);
    void ApplyInteractiveBrowserSettings();
    void CommitBrowserFlag(const std::function<void(revia::browser::BrowserSettings&)>& change);
    void ApplyCameraSettings();
    void ApplyProcessSettings();
    void CommitProcessFlag(const std::function<void(revia::process::ProcessSettings&)>& change);
    void ApplyDesktopControlSettings();
    void ToggleDesktopStop();
    // Puts the switch back off and asks; on "Turn on" the switch goes on again and apply runs.
    // Window-modal and non-blocking, so no nested event loop waits on the answer.
    void ConfirmTurnOn(ToggleSwitch* toggle, const QString& explanation, std::function<void()> apply);
    void ShowInternetResult(const QString& text, bool error);
    [[nodiscard]] QString CompanionName() const;
    [[nodiscard]] QString Warning(const ToggleSwitch* toggle) const;

    revia::runtime::ReviaSession& session;
    DiscoveryRequest requestDiscovery;
    QPointer<QWidget> internetGroup;
    bool internetDetached = false;
    QLabel* internetStatusLabel = nullptr;
    QTreeWidget* approvedTree = nullptr;
    QTableWidget* discoveredTable = nullptr;
    QLabel* discoveryLabel = nullptr;
    QLabel* statusLabel = nullptr;
    QComboBox* taskApprovalMode = nullptr;
    ToggleSwitch* internetCheck = nullptr;
    ToggleSwitch* automaticLookupCheck = nullptr;
    ToggleSwitch* visibleBrowserCheck = nullptr;
    ToggleSwitch* autonomousResearchCheck = nullptr;
    ToggleSwitch* cameraCheck = nullptr;
    ToggleSwitch* autonomousCameraCheck = nullptr;
    ToggleSwitch* processCheck = nullptr;
    ToggleSwitch* taskProcessCheck = nullptr;
    ToggleSwitch* commandInterpreterCheck = nullptr;
    QPlainTextEdit* approvedExecutablesEdit = nullptr;
    QLabel* processStatusLabel = nullptr;
    QPushButton* applyProcessButton = nullptr;
    QWidget* processListFooter = nullptr;
    ToggleSwitch* interactiveBrowserCheck = nullptr;
    ToggleSwitch* browserNavigateCheck = nullptr;
    ToggleSwitch* browserInteractCheck = nullptr;
    ToggleSwitch* taskBrowserCheck = nullptr;
    ToggleSwitch* browserLoopbackCheck = nullptr;
    QPlainTextEdit* approvedOriginsEdit = nullptr;
    QLabel* browserStatusLabel = nullptr;
    QPushButton* applyInteractiveBrowserButton = nullptr;
    QWidget* browserListFooter = nullptr;
    ToggleSwitch* pointerCheck = nullptr;
    ToggleSwitch* keyboardCheck = nullptr;
    ToggleSwitch* launchCheck = nullptr;
    ToggleSwitch* rawCoordinateCheck = nullptr;
    ToggleSwitch* visualTargetCheck = nullptr;
    ToggleSwitch* wholeDesktopCheck = nullptr;
    ToggleSwitch* commandSurfaceCheck = nullptr;
    ToggleSwitch* autonomousDesktopCheck = nullptr;
    QPushButton* desktopStopButton = nullptr;
    QPushButton* approveDiscoveredButton = nullptr;
    bool refreshing = false;
    bool processSettingsDirty = false;
    bool browserSettingsDirty = false;
    std::string discoveredApplication;
};
