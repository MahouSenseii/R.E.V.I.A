#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTreeWidget;
class ToggleSwitch;

// Presentation and explicit permission edits only. ReviaSession remains the owner of
// policy persistence/reload and Windows remains the source of discovered identities.
class CapabilityPanel final : public QWidget
{
public:
    using DiscoveryRequest = std::function<void()>;

    CapabilityPanel(
        revia::runtime::ReviaSession& session,
        DiscoveryRequest discoveryRequest,
        QWidget* parent = nullptr);

    void Refresh();
    void ShowDiscovery(
        const revia::actions::windows::ApplicationControlInventory& inventory);
    void SetStatus(const QString& text, bool error = false);

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
    void ApplyCameraSettings();
    void ApplyDesktopControlSettings();
    void ToggleDesktopStop();

    revia::runtime::ReviaSession& session;
    DiscoveryRequest requestDiscovery;
    QTreeWidget* approvedTree = nullptr;
    QTableWidget* discoveredTable = nullptr;
    QLabel* discoveryLabel = nullptr;
    QLabel* statusLabel = nullptr;
    ToggleSwitch* internetCheck = nullptr;
    ToggleSwitch* automaticLookupCheck = nullptr;
    ToggleSwitch* visibleBrowserCheck = nullptr;
    ToggleSwitch* autonomousResearchCheck = nullptr;
    ToggleSwitch* cameraCheck = nullptr;
    ToggleSwitch* autonomousCameraCheck = nullptr;
    ToggleSwitch* pointerCheck = nullptr;
    ToggleSwitch* keyboardCheck = nullptr;
    ToggleSwitch* launchCheck = nullptr;
    ToggleSwitch* rawCoordinateCheck = nullptr;
    ToggleSwitch* wholeDesktopCheck = nullptr;
    ToggleSwitch* commandSurfaceCheck = nullptr;
    ToggleSwitch* autonomousDesktopCheck = nullptr;
    QPushButton* desktopStopButton = nullptr;
    QPushButton* approveDiscoveredButton = nullptr;
    bool refreshing = false;
    std::string discoveredApplication;
};
