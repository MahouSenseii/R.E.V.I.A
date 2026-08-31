#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

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

    void Refresh();

private:
    void CaptureCameraFrame();
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

    QLabel* perceptionSummary = nullptr;
};
