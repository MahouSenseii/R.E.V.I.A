#include "visionPanel.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

VisionPanel::VisionPanel(revia::runtime::ReviaSession& inputSession, QWidget* parent)
    : QWidget(parent),
      session(inputSession)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* title = new QLabel("Vision", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);

    auto* explanation = new QLabel(
        "What Revia can see, and what she is permitted to look at. Screen capture, camera "
        "capture, and ambient observation are separate permissions, all off unless granted "
        "under Permissions. Seeing is not acting: a frame taken here grants no authority "
        "over whatever it happens to contain.", this);
    explanation->setWordWrap(true);
    explanation->setObjectName("secondaryText");
    layout->addWidget(explanation);

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("visionStatus");
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto* screenGroup = new QGroupBox("Screens", this);
    auto* screenLayout = new QVBoxLayout(screenGroup);
    screenSummary = new QLabel(screenGroup);
    screenSummary->setObjectName("secondaryText");
    screenSummary->setWordWrap(true);
    screenLayout->addWidget(screenSummary);
    monitorTable = new QTableWidget(screenGroup);
    monitorTable->setColumnCount(4);
    monitorTable->setHorizontalHeaderLabels({"Display", "Size", "Position", "Primary"});
    monitorTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
    {
        monitorTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    monitorTable->verticalHeader()->setVisible(false);
    monitorTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    monitorTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    monitorTable->setAlternatingRowColors(true);
    monitorTable->verticalHeader()->setDefaultSectionSize(26);
    monitorTable->setMaximumHeight(160);
    screenLayout->addWidget(monitorTable);
    layout->addWidget(screenGroup);

    auto* cameraGroup = new QGroupBox("Camera", this);
    auto* cameraLayout = new QVBoxLayout(cameraGroup);
    cameraSummary = new QLabel(cameraGroup);
    cameraSummary->setObjectName("secondaryText");
    cameraSummary->setWordWrap(true);
    cameraLayout->addWidget(cameraSummary);
    auto* cameraRow = new QHBoxLayout();
    cameraCombo = new QComboBox(cameraGroup);
    cameraCombo->setMinimumWidth(240);
    cameraCombo->setMaximumWidth(460);
    cameraRow->addWidget(cameraCombo);
    captureButton = new QPushButton("Take one frame", cameraGroup);
    cameraRow->addWidget(captureButton);
    cameraRow->addStretch();
    cameraLayout->addLayout(cameraRow);
    preview = new QLabel(cameraGroup);
    preview->setMinimumHeight(220);
    preview->setAlignment(Qt::AlignCenter);
    preview->setObjectName("secondaryText");
    preview->setText("No frame taken this session.");
    cameraLayout->addWidget(preview, 1);
    layout->addWidget(cameraGroup, 1);

    perceptionSummary = new QLabel(this);
    perceptionSummary->setObjectName("secondaryText");
    perceptionSummary->setWordWrap(true);
    layout->addWidget(perceptionSummary);

    connect(captureButton, &QPushButton::clicked, this, [this]() { CaptureCameraFrame(); });
    connect(cameraCombo, &QComboBox::currentIndexChanged, this, [this](const int index)
    {
        if (index < 0) return;
        // Stored by symbolic link, not by position: an index saved today points at a
        // different lens as soon as anything else is plugged in.
        QSettings().setValue("vision/cameraLink", cameraCombo->itemData(index).toString());
    });
    Refresh();
}

void VisionPanel::Refresh()
{
    RenderMonitors();
    RenderCameras();

    const bool screenReady = session.IsVisionAvailable();
    const bool cameraReady = session.IsCameraAvailable();
    // Stated separately rather than as one "vision" flag, because they are separate
    // permissions and a user turning one on should not be told the other is live.
    statusLabel->setText(
        QString("Screen vision: ") + (screenReady ? "ready" : "off or unavailable") +
        "   Camera: " + (cameraReady ? "allowed" : "off") +
        "   Ambient observation: " + (session.IsPerceptionEnabled()
            ? (session.IsPerceptionPaused() ? "paused" : "watching") : "off"));

    perceptionSummary->setText(QString::fromStdString(session.PerceptionStatus()));
}

void VisionPanel::RenderMonitors()
{
    // Enumeration reads the display topology and captures no pixels, so it stays
    // available regardless of whether screen vision is permitted.
    const std::vector<revia::vision::MonitorDescriptor> monitors =
        session.Monitors();
    screenSummary->setText(monitors.empty()
        ? QString("No displays were reported.")
        : QString::number(static_cast<int>(monitors.size())) +
            (monitors.size() == 1 ? " display attached." : " displays attached."));

    monitorTable->setRowCount(static_cast<int>(monitors.size()));
    for (std::size_t index = 0; index < monitors.size(); ++index)
    {
        const revia::vision::MonitorDescriptor& monitor = monitors[index];
        const int row = static_cast<int>(index);
        const QString name = monitor.deviceName.empty()
            ? QString("Display %1").arg(monitor.index)
            : QString::fromStdString(monitor.deviceName);
        monitorTable->setItem(row, 0, new QTableWidgetItem(name));
        monitorTable->setItem(row, 1, new QTableWidgetItem(
            QString::number(monitor.Width()) + " x " + QString::number(monitor.Height())));
        monitorTable->setItem(row, 2, new QTableWidgetItem(
            QString::number(monitor.left) + ", " + QString::number(monitor.top)));
        monitorTable->setItem(row, 3, new QTableWidgetItem(
            monitor.primary ? "yes" : ""));
    }
}

void VisionPanel::RenderCameras()
{
    const std::vector<revia::vision::CameraDescriptor> cameras = session.Cameras();
    const bool allowed = session.IsCameraAvailable();

    cameraSummary->setText(cameras.empty()
        ? QString("No camera is attached, or Windows privacy settings are blocking "
                  "access for desktop apps.")
        : allowed
            ? QString::number(static_cast<int>(cameras.size())) +
                (cameras.size() == 1 ? " camera available." : " cameras available.") +
                "  A capture opens the device, takes one frame, and closes it again."
            : QString::number(static_cast<int>(cameras.size())) +
                " camera(s) detected, but camera access is off. Enable it under "
                "Permissions before Revia can look.");

    // The saved camera outranks the in-session one, so a restart restores what the
    // user chose rather than whatever enumerates first.
    const QString inSession = cameraCombo->currentData().toString();
    const QString saved = QSettings().value("vision/cameraLink", QString()).toString();
    const QString previous = inSession.isEmpty() ? saved : inSession;

    const QSignalBlocker blocker(cameraCombo);
    cameraCombo->clear();
    for (const revia::vision::CameraDescriptor& camera : cameras)
    {
        cameraCombo->addItem(
            QString::fromStdString(camera.name),
            QString::fromStdString(camera.symbolicLink));
    }
    const int index = cameraCombo->findData(previous);
    if (index >= 0)
    {
        cameraCombo->setCurrentIndex(index);
    }
    else if (!saved.isEmpty() && !cameras.empty())
    {
        // Said out loud rather than quietly reverting to camera one. A selection that
        // silently moves to a different lens is the failure this reporting prevents.
        SetStatus("The camera saved from a previous session is not attached. Pick a "
                  "camera; Revia will not use a different one on its own.", true);
    }
    cameraCombo->setEnabled(!cameras.empty());
    captureButton->setEnabled(allowed && !cameras.empty());
}

void VisionPanel::CaptureCameraFrame()
{
    captureButton->setEnabled(false);
    SetStatus("Opening the camera for one frame...");
    // Synchronous and deliberately brief. A capture is around a second and a half, and
    // the camera light going out is the signal that it is over.
    revia::vision::CameraSelection selection;
    selection.symbolicLink = cameraCombo->currentData().toString().toStdString();
    selection.index = cameraCombo->currentIndex() + 1;
    // A person picked this from a list. That is what makes substitution unacceptable
    // rather than merely undesirable.
    selection.explicitChoice = !selection.symbolicLink.empty();
    const revia::vision::CameraFrame frame =
        session.CaptureCameraFrame(false, selection);
    captureButton->setEnabled(true);

    if (!frame.succeeded)
    {
        preview->setText("No frame. " + QString::fromStdString(frame.reason));
        SetStatus(QString::fromStdString(frame.reason), true);
        // The device list is re-read on failure so a camera that was unplugged stops
        // being offered, rather than staying selectable and failing again.
        Refresh();
        return;
    }

    QPixmap image(QString::fromStdString(frame.path.string()));
    if (image.isNull())
    {
        preview->setText("The frame was written but could not be displayed.");
        SetStatus("The frame was captured but could not be loaded for preview.", true);
        return;
    }
    preview->setPixmap(image.scaled(
        preview->width(), preview->height(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    SetStatus(QString::fromStdString(frame.reason) + "  " +
        QString::number(frame.width) + "x" + QString::number(frame.height) + ", " +
        QString::number(static_cast<int>(frame.elapsedMilliseconds)) + "ms, " +
        QString::number(frame.warmupFramesDiscarded) + " warm-up frames discarded.");
}

void VisionPanel::SetStatus(const QString& text, const bool error)
{
    statusLabel->setText(text);
    statusLabel->setProperty("error", error);
    statusLabel->style()->unpolish(statusLabel);
    statusLabel->style()->polish(statusLabel);
}
