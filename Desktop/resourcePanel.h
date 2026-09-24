#pragma once

#include "Core/preferenceStore.h"
#include "Runtime/runtimeEvents.h"

#include <QWidget>

#include <functional>
#include <map>
#include <string>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

// View of the startup inventory, immutable resource plan, and live readings. Its one
// control forwards a durable voice-placement preference to the runtime owner; it never
// starts or moves a worker itself, and the new plan is applied on the next launch.
class ResourcePanel final : public QWidget
{
public:
    using VoiceDeviceHandler = std::function<revia::core::PreferenceResult(const std::string&)>;

    explicit ResourcePanel(VoiceDeviceHandler voiceDeviceHandler, QWidget* parent = nullptr);

    void Observe(const revia::runtime::RuntimeEvent& event);
    void SetVoiceDevicePreference(const std::string& device);

private:
    struct UsageRow
    {
        int row = 0;
        QProgressBar* bar = nullptr;
    };

    int EnsureHardwareRow(const QString& name);
    int EnsureAssignmentRow(const QString& workload);
    UsageRow& EnsureUsageRow(const QString& label);
    void ApplyUsage(const revia::runtime::RuntimeEvent& event);
    void AddVoiceGpuChoice(const QString& backendId, const QString& name);
    static void SetCell(QTableWidget* table, int row, int column, const QString& text);
    // Grows a table to exactly the height its rows need. Three tables sharing one
    // stretchy column is what made the section headings overlap them: each asked for the
    // same space, and none of them fit. The page scrolls instead.
    static void FitToContents(QTableWidget* table);

    QTableWidget* usageTable = nullptr;
    QTableWidget* hardwareTable = nullptr;
    QTableWidget* assignmentTable = nullptr;
    QComboBox* voiceDeviceCombo = nullptr;
    QPushButton* saveVoiceDeviceButton = nullptr;
    QLabel* voiceDeviceStatus = nullptr;
    VoiceDeviceHandler onVoiceDeviceRequested;
    std::map<QString, UsageRow> usageRows;
    std::map<QString, int> hardwareRows;
    std::map<QString, int> assignmentRows;
};
