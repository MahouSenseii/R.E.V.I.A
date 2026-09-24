#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

#include <thread>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

// What Revia can see, and what she is allowed to look at.
//
// Screen capture, camera capture, and ambient observation are three separate
// capabilities that had no single place showing their state. Seeing is not acting:
// nothing on this panel can invoke a control or reach a file, and a frame captured here
// grants no authority over whatever it happens to contain.
class VisionPanel final : public QWidget
{
public:
    explicit VisionPanel(revia::runtime::ReviaSession& session, QWidget* parent = nullptr);
    ~VisionPanel() override;

    void Refresh();

private:
    void CaptureCameraFrame();
    void ShowCameraFrame(const revia::vision::CameraFrame& frame);
    void RenderMonitors();
    void RenderCameras();
    void SetStatus(const QString& text, bool error = false);

    revia::runtime::ReviaSession& session;

    QLabel* statusLabel = nullptr;
    QLabel* screenSummary = nullptr;
    QTableWidget* monitorTable = nullptr;

    QLabel* cameraSummary = nullptr;
    QComboBox* cameraCombo = nullptr;
    QPushButton* captureButton = nullptr;
    QLabel* preview = nullptr;
    // A capture owns the camera for over a second, so it runs off the GUI thread.
    // Refresh must not hand the button back while that capture is still running.
    std::jthread captureWorker;
    bool cameraCaptureRunning = false;

    QLabel* perceptionSummary = nullptr;
};
