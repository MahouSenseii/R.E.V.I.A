#pragma once

#include "Runtime/reviaSession.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <initializer_list>
#include <vector>

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
    // Words every row's label and help, and the notes that name her, with this name: the active
    // companion's display name, known only once her session has started. Refresh picks up a
    // changed name by itself.
    void SetCompanionName(const QString& name);
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
    // The switch that changed, applied on top of the saved permissions; see ApplyCameraSettings.
    void ApplyInternetSettings(const ToggleSwitch* changed);
    void ApplyBrowserSettings(const ToggleSwitch* changed);
    QFrame* CreateInternetCard();
    QFrame* CreateInteractiveBrowserCard();
    QWidget* CreateListFooter(QPushButton*& apply, const QString& applyName, std::function<void()> onApply,
        std::function<void()> onDiscard);
    QFrame* MakeRow(ToggleSwitch* toggle, const QString& tip = QString(), bool nested = false, const QString& riskText = QString());
    void RenderRowText(std::size_t index);
    // "Turn on A first", "Turn on A and B first" (or "A or B" when any one will do): the switches
    // a row needs that are still off.
    [[nodiscard]] QString NeedsFirst(std::initializer_list<const ToggleSwitch*> needed, bool anyOne = false) const;
    [[nodiscard]] QString Label(const ToggleSwitch* toggle) const;
    void RefreshInteractiveBrowserSettings(const revia::actions::CapabilitySettings& settings);
    void ApplyInteractiveBrowserSettings();
    void CommitBrowserFlag(const std::function<void(revia::browser::BrowserSettings&)>& change);
    void ApplyCameraSettings(const ToggleSwitch* changed);
    void ApplyProcessSettings();
    void CommitProcessFlag(const std::function<void(revia::process::ProcessSettings&)>& change);
    void ApplyDesktopControlSettings(const ToggleSwitch* changed);
    void ToggleDesktopStop();
    // Puts the switch back off and asks; on "Turn on" the switch goes on again and apply runs.
    // Window-modal and non-blocking, so no nested event loop waits on the answer. note is an
    // optional short sentence after the switch's warning.
    void ConfirmTurnOn(ToggleSwitch* toggle, const QString& note, std::function<void()> apply);
    // The shared question: the question, its consequence, and Turn on or Cancel (the default);
    // accepted runs only on Turn on.
    void AskToConfirm(const QString& question, const QString& consequence, std::function<void()> accepted);
    void ShowInternetResult(const QString& text, bool error);
    [[nodiscard]] QString CompanionName() const;
    [[nodiscard]] QString Warning(const ToggleSwitch* toggle) const;

    // What a row says, kept so a new companion name can word it again.
    struct RowText
    {
        ToggleSwitch* toggle = nullptr;
        QFrame* row = nullptr;
        QLabel* label = nullptr;
        QLabel* help = nullptr;
        QLabel* detail = nullptr;
        QString tip;
        QStringList badges;
    };

    revia::runtime::ReviaSession& session;
    DiscoveryRequest requestDiscovery;
    QString companionName;
    std::vector<RowText> rowTexts;
    QLabel* gatedNotice = nullptr;
    QLabel* approvedHelp = nullptr;
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
