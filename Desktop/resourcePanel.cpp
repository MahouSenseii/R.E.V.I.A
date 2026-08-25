#include "resourcePanel.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    constexpr int SectionSpacing = 22;
    constexpr int BarColumnWidth = 150;
    constexpr int RowHeight = 34;

    void ConfigureTable(QTableWidget* table)
    {
        table->verticalHeader()->setVisible(false);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->setFocusPolicy(Qt::NoFocus);
        // The page scrolls, not the individual tables. A table with its own scrollbar
        // hides rows inside a page that already has room to show them.
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        // Rows are a fixed height and text does not wrap, and that is load-bearing rather
        // than a style choice. Content-sized rows plus a stretching last column plus a
        // height driven back into the layout is a cycle: resizing rows changes the widget
        // height, which re-lays out the page, which changes the stretch column's width,
        // which re-wraps the text, which resizes the rows. Refreshed every couple of
        // seconds that recursion overflows the stack and takes the process with it,
        // leaving no crash dump because a stack overflow in a GUI app rarely produces one.
        // Long text elides and carries a tooltip instead.
        table->setWordWrap(false);
        table->setTextElideMode(Qt::ElideRight);
        table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->verticalHeader()->setDefaultSectionSize(RowHeight);
    }

    QString MemoryLabel(const revia::runtime::RuntimeEvent& event)
    {
        if (event.totalMemoryMiB == 0)
        {
            return QStringLiteral("-");
        }
        return QString::number(event.availableMemoryMiB) + QStringLiteral(" / ") +
            QString::number(event.totalMemoryMiB) + QStringLiteral(" MiB free / total");
    }

    // Value and unit stay together so a converted number can never be drawn beside the
    // wrong label, and both sides of a comparison use the same unit.
    QString Scaled(const double mebibytes, const bool gibibytes)
    {
        if (!gibibytes)
        {
            return QString::number(mebibytes, 'f', 0);
        }
        // A reading far below the row's unit still has to be legible. Rendering 40 MiB as
        // "0.0 GiB" beside a 12 GiB budget reads as nothing at all being used.
        const double converted = mebibytes / 1024.0;
        return QString::number(converted, 'f', converted < 1.0 ? 2 : 1);
    }

    bool IsThreadMeter(const revia::runtime::RuntimeEvent& event)
    {
        return event.usageUnit == "threads";
    }

    QString FormatAmount(const revia::runtime::RuntimeEvent& event, const double value)
    {
        if (IsThreadMeter(event))
        {
            return QString::number(value, 'f', value < 10.0 ? 1 : 0);
        }
        const double largest = std::max(
            {event.usedAmount, event.budgetAmount, event.capacityAmount});
        const bool gibibytes = largest >= 1024.0;
        return Scaled(value, gibibytes) +
            (gibibytes ? QStringLiteral(" GiB") : QStringLiteral(" MiB"));
    }

    QString InUseText(const revia::runtime::RuntimeEvent& event)
    {
        const QString amount = FormatAmount(event, event.usedAmount);
        return IsThreadMeter(event) ? amount + QStringLiteral(" threads") : amount;
    }

    QString BudgetText(const revia::runtime::RuntimeEvent& event)
    {
        if (event.budgetAmount <= 0.0)
        {
            return QStringLiteral("no budget set");
        }
        QString text = FormatAmount(event, event.budgetAmount);
        if (event.capacityAmount > 0.0)
        {
            text += QStringLiteral(" of ") + FormatAmount(event, event.capacityAmount) +
                (IsThreadMeter(event)
                    ? QStringLiteral(" logical")
                    : QStringLiteral(" installed"));
        }
        return text;
    }
}

ResourcePanel::ResourcePanel(VoiceDeviceHandler voiceDeviceHandler, QWidget* parent)
    : QWidget(parent), onVoiceDeviceRequested(std::move(voiceDeviceHandler))
{
    // The panel is a scrolling page. Its three sections are sized by their contents, so
    // the window height decides how much is visible rather than how much is legible.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);

    auto* page = new QWidget(scroll);
    scroll->setWidget(page);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(2, 14, 12, 20);
    layout->setSpacing(8);

    const auto addSection = [&](const QString& title, const QString& hint)
    {
        auto* heading = new QLabel(title, page);
        heading->setObjectName("sectionTitle");
        // Breathing room above each heading, so a section reads as its own block rather
        // than as more rows of the table above it.
        heading->setContentsMargins(0, SectionSpacing, 0, 4);
        layout->addWidget(heading);
        if (hint.isEmpty())
        {
            return;
        }
        auto* description = new QLabel(hint, page);
        description->setWordWrap(true);
        description->setObjectName("secondaryText");
        description->setContentsMargins(0, 0, 0, 6);
        layout->addWidget(description);
    };

    auto* summary = new QLabel(
        "Revia rebuilds this plan on every startup. Independent long-lived workers are "
        "placed on different devices when possible; RAM is a bounded cache and model "
        "mapping layer, not a substitute for GPU memory.", page);
    summary->setWordWrap(true);
    summary->setObjectName("secondaryText");
    layout->addWidget(summary);

    addSection("Voice generation device",
        "Auto chooses a safe device on every startup. CPU protects GPU memory but is "
        "slower. Selecting a GPU reserves room for Qwen3-TTS and may move more chat "
        "layers to CPU. Changes apply after restarting Revia.");
    auto* voiceDeviceRow = new QHBoxLayout();
    voiceDeviceCombo = new QComboBox(page);
    voiceDeviceCombo->addItem("Auto (recommended)", "auto-secondary");
    voiceDeviceCombo->addItem("CPU", "cpu");
    saveVoiceDeviceButton = new QPushButton("Save voice device", page);
    saveVoiceDeviceButton->setEnabled(static_cast<bool>(onVoiceDeviceRequested));
    voiceDeviceRow->addWidget(voiceDeviceCombo, 1);
    voiceDeviceRow->addWidget(saveVoiceDeviceButton);
    layout->addLayout(voiceDeviceRow);
    voiceDeviceStatus = new QLabel(
        "The resolved device remains visible in the assignment table below.", page);
    voiceDeviceStatus->setWordWrap(true);
    voiceDeviceStatus->setObjectName("secondaryText");
    layout->addWidget(voiceDeviceStatus);
    connect(saveVoiceDeviceButton, &QPushButton::clicked, this, [this]()
    {
        if (!onVoiceDeviceRequested)
        {
            return;
        }
        const std::string device = voiceDeviceCombo->currentData().toString().toStdString();
        const revia::core::PreferenceResult result = onVoiceDeviceRequested(device);
        voiceDeviceStatus->setText(QString::fromStdString(result.message));
        voiceDeviceStatus->setProperty("error", !result.succeeded);
        voiceDeviceStatus->style()->unpolish(voiceDeviceStatus);
        voiceDeviceStatus->style()->polish(voiceDeviceStatus);
    });

    addSection("Live usage against the plan",
        "Sampled continuously and compared with the budget the plan set aside. Video "
        "memory is the system-wide figure for the adapter, because the model weights "
        "live in a worker process; RAM and CPU cover Revia and every process it "
        "started. Nothing here changes the plan.");

    usageTable = new QTableWidget(0, 5, page);
    usageTable->setHorizontalHeaderLabels(
        {"Resource", "In use", "Budget", "Against budget", "Detail"});
    ConfigureTable(usageTable);
    usageTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    usageTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    usageTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    usageTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    usageTable->horizontalHeader()->resizeSection(3, BarColumnWidth);
    usageTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    layout->addWidget(usageTable);

    addSection("Detected hardware and budgets", {});
    hardwareTable = new QTableWidget(0, 5, page);
    hardwareTable->setHorizontalHeaderLabels(
        {"Resource", "Identity", "Startup capacity", "Configured ceiling", "Detail"});
    ConfigureTable(hardwareTable);
    hardwareTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    layout->addWidget(hardwareTable);

    addSection("Resolved workload assignments", {});
    assignmentTable = new QTableWidget(0, 3, page);
    assignmentTable->setHorizontalHeaderLabels({"Workload", "Planned device", "Strategy"});
    ConfigureTable(assignmentTable);
    assignmentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    assignmentTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    assignmentTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(assignmentTable);

    layout->addStretch(1);
}

void ResourcePanel::AddVoiceGpuChoice(const QString& backendId, const QString& name)
{
    if (!backendId.startsWith(QStringLiteral("CUDA")))
    {
        return;
    }
    const int existing = voiceDeviceCombo->findData(backendId);
    const QString label = backendId + QStringLiteral(" - ") + name;
    if (existing >= 0)
    {
        voiceDeviceCombo->setItemText(existing, label);
        return;
    }
    voiceDeviceCombo->addItem(label, backendId);
}

void ResourcePanel::SetVoiceDevicePreference(const std::string& device)
{
    const QString wanted = QString::fromStdString(
        device == "auto" ? std::string("auto-secondary") : device);
    int index = voiceDeviceCombo->findData(wanted);
    if (index < 0 && wanted.startsWith(QStringLiteral("CUDA")))
    {
        voiceDeviceCombo->addItem(wanted + QStringLiteral(" - selected GPU"), wanted);
        index = voiceDeviceCombo->count() - 1;
    }
    if (index >= 0)
    {
        voiceDeviceCombo->setCurrentIndex(index);
    }
}

void ResourcePanel::FitToContents(QTableWidget* table)
{
    // Derived from a fixed row height rather than measured from the rows, so asking for
    // the height cannot itself change the height. sizeHint for the header because it may
    // not have been laid out the first time a row arrives.
    const int header = std::max(
        table->horizontalHeader()->height(),
        table->horizontalHeader()->sizeHint().height());
    const int height = header + 2 * table->frameWidth() + 2 +
        table->rowCount() * RowHeight;

    // Only when it actually changes. This runs on every live-usage sample, and setting the
    // same fixed height every two seconds still invalidates the layout every two seconds.
    if (table->height() != height || table->minimumHeight() != height)
    {
        table->setFixedHeight(height);
    }
}

int ResourcePanel::EnsureHardwareRow(const QString& name)
{
    const auto found = hardwareRows.find(name);
    if (found != hardwareRows.end())
    {
        return found->second;
    }
    const int row = hardwareTable->rowCount();
    hardwareTable->insertRow(row);
    hardwareRows.emplace(name, row);
    SetCell(hardwareTable, row, 0, name);
    return row;
}

int ResourcePanel::EnsureAssignmentRow(const QString& workload)
{
    const auto found = assignmentRows.find(workload);
    if (found != assignmentRows.end())
    {
        return found->second;
    }
    const int row = assignmentTable->rowCount();
    assignmentTable->insertRow(row);
    assignmentRows.emplace(workload, row);
    SetCell(assignmentTable, row, 0, workload);
    return row;
}

ResourcePanel::UsageRow& ResourcePanel::EnsureUsageRow(const QString& label)
{
    const auto found = usageRows.find(label);
    if (found != usageRows.end())
    {
        return found->second;
    }
    const int row = usageTable->rowCount();
    usageTable->insertRow(row);
    SetCell(usageTable, row, 0, label);

    // The bar lives inside a container so it keeps its own height and margins whatever
    // the row grows to when the detail column wraps.
    auto* holder = new QWidget(usageTable);
    auto* holderLayout = new QHBoxLayout(holder);
    holderLayout->setContentsMargins(8, 4, 8, 4);
    auto* bar = new QProgressBar(holder);
    bar->setRange(0, 100);
    bar->setTextVisible(true);
    bar->setFixedHeight(20);
    holderLayout->addWidget(bar);
    usageTable->setCellWidget(row, 3, holder);

    return usageRows.emplace(label, UsageRow{row, bar}).first->second;
}

void ResourcePanel::SetCell(
    QTableWidget* table,
    const int row,
    const int column,
    const QString& text)
{
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr)
    {
        item = new QTableWidgetItem();
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table->setItem(row, column, item);
    }
    item->setText(text);
    // Wrapping keeps a long detail readable, and the tooltip means a column squeezed by a
    // narrow window still gives up its full text rather than ending in an ellipsis.
    item->setToolTip(text);
}

void ResourcePanel::ApplyUsage(const revia::runtime::RuntimeEvent& event)
{
    const QString label = QString::fromStdString(event.component);
    UsageRow& usage = EnsureUsageRow(label);

    const int percent = event.budgetAmount > 0.0
        ? static_cast<int>(std::lround(100.0 * event.usedAmount / event.budgetAmount))
        : 0;
    const bool overBudget = event.usageMeasured && event.budgetAmount > 0.0 && percent > 100;

    if (!event.usageMeasured)
    {
        // An unmeasured reading is shown as unmeasured. Drawing a zero would say the
        // resource is idle, which is a different and wrong claim.
        SetCell(usageTable, usage.row, 1, QStringLiteral("not measured"));
        usage.bar->setValue(0);
        usage.bar->setFormat(QStringLiteral("unavailable"));
    }
    else
    {
        SetCell(usageTable, usage.row, 1, InUseText(event));
        usage.bar->setValue(std::clamp(percent, 0, 100));
        // The bar saturates at full; the number does not, so a reading past its budget
        // still says by how much rather than looking merely full.
        usage.bar->setFormat(event.budgetAmount > 0.0
            ? QString::number(percent) + QStringLiteral("%")
            : QStringLiteral("no budget"));
    }
    usage.bar->setProperty("overBudget", overBudget);
    usage.bar->setProperty("unavailable", !event.usageMeasured);
    usage.bar->style()->unpolish(usage.bar);
    usage.bar->style()->polish(usage.bar);

    SetCell(usageTable, usage.row, 2, BudgetText(event));
    SetCell(usageTable, usage.row, 4, QString::fromStdString(event.message));
    FitToContents(usageTable);
}

void ResourcePanel::Observe(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind != revia::runtime::RuntimeEventKind::ResourceStatus ||
        event.component.empty())
    {
        return;
    }
    if (event.phase == "Usage")
    {
        ApplyUsage(event);
        return;
    }

    const QString component = QString::fromStdString(event.component);
    if (event.phase == "GPU")
    {
        AddVoiceGpuChoice(component, QString::fromStdString(event.resource));
    }
    if (event.phase == "Assignment")
    {
        const int row = EnsureAssignmentRow(component);
        SetCell(assignmentTable, row, 1, QString::fromStdString(event.resource));
        SetCell(assignmentTable, row, 2, QString::fromStdString(event.message));
        FitToContents(assignmentTable);
        return;
    }

    const int row = EnsureHardwareRow(component);
    SetCell(hardwareTable, row, 1, QString::fromStdString(event.resource));
    SetCell(hardwareTable, row, 2, MemoryLabel(event));
    SetCell(hardwareTable, row, 3, event.allocatedMemoryMiB > 0
        ? QString::number(event.allocatedMemoryMiB) + QStringLiteral(" MiB")
        : QStringLiteral("role budget"));
    SetCell(hardwareTable, row, 4, QString::fromStdString(event.message));
    FitToContents(hardwareTable);
}
