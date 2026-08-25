#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTreeWidget;

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

private:
    void AddApplicationManually();
    void ApproveSelectedDiscoveredControls();
    void RemoveSelectedPermission();
    void ApplyInternetSettings();
    void ApplyBrowserSettings();

    revia::runtime::ReviaSession& session;
    DiscoveryRequest requestDiscovery;
    QTreeWidget* approvedTree = nullptr;
    QTableWidget* discoveredTable = nullptr;
    QLabel* discoveryLabel = nullptr;
    QLabel* statusLabel = nullptr;
    QCheckBox* internetCheck = nullptr;
    QCheckBox* automaticLookupCheck = nullptr;
    QCheckBox* visibleBrowserCheck = nullptr;
    QCheckBox* autonomousResearchCheck = nullptr;
    QPushButton* approveDiscoveredButton = nullptr;
    bool refreshing = false;
    std::string discoveredApplication;
};
