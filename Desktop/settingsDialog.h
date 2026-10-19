#pragma once

#include "settingsTypes.h"

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QEvent;
class QFrame;
class QHideEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QStackedWidget;
class QVBoxLayout;
class QWidget;
class ToggleSwitch;

namespace revia::desktop
{
// The one Settings window: a searchable list of sections on the left and, on the right, a page of
// labelled switches for every catalog row the dialog hosts, plus the panels and custom rows the
// window places into it. Created once with the window and never deleted, so the window's members
// can point at its switches. It never touches ReviaSession: Shell rows go to QSettings and every
// other store goes through the SettingsBackend, whose writes are asynchronous.
class SettingsDialog final : public QDialog
{
public:
    SettingsDialog(SettingsBackend backend, SettingsHooks hooks, QWidget* parent);
    ~SettingsDialog() override;

    // The switch for a catalog key or objectName: a dialog row's own switch, or a hosted panel's
    // toggle with that objectName. Null when neither exists.
    [[nodiscard]] QCheckBox* Switch(const std::string& keyOrObjectName) const;
    // Changes what a row shows, and the rows that depend on it, without writing anything.
    void SetValue(const std::string& key, bool value);
    // A status line that stays until replaced or cleared with an empty text. tone: success, error,
    // info or warning.
    void SetRowStatus(const std::string& key, const QString& text, const QString& tone);
    // Re-reads every value (QSettings, then the backend in one call) and the theme and text size.
    void Refresh();
    // Releases every row still waiting for a write whose answer will never come, because the
    // session it was made for has been retired: the switch is enabled again and "Saving…" goes.
    // The next Refresh shows what was actually saved.
    void AbandonPendingWrites();
    void SetCompanionName(const QString& name);
    void ShowSection(SettingsSection section);
    // Accepts a section name or id ("Voice", "voice"), or a setting key or objectName, which also
    // focuses that setting. False when nothing matches.
    bool ShowSection(const QString& nameOrKey);
    // Shows the dialog on the setting's section, scrolls to it and highlights its row for 1.2 s.
    void FocusSetting(const std::string& keyOrObjectName);
    [[nodiscard]] QWidget* SectionPage(SettingsSection section) const;
    [[nodiscard]] QList<QWidget*> SectionPages() const;
    // A row without a catalog switch: label and description on the left, control on the right
    // (below the text when it is wide). requiresAll keys must be on for the control to be enabled.
    QWidget* AddCustomRow(SettingsSection section, const QString& group, const QString& label, const QString& description,
        QWidget* control, const QString& rowObjectName, std::vector<std::string> requiresAll = {});
    QWidget* AddLinkRow(SettingsSection section, const QString& group, const QString& label, const QString& description,
        const QString& buttonText, std::function<void()> onClick, const QString& rowObjectName);
    // Puts a widget the caller owns into a section, at a position among that section's groups (0 is
    // the top, -1 the end). A widget already in the slot is taken out but not deleted.
    void SetHostedWidget(SettingsSection section, const QString& slot, QWidget* widget, int position = -1);
    void SetLocked(bool locked);
    void SetResizeHook(std::function<void(QSize contentSize)> hook);
    // Rows whose saved value differs from the one in effect and that wait for a restart.
    [[nodiscard]] int PendingRestartCount() const;
    // Every row the dialog built a switch for, tool-server rows included, with "{name}" unexpanded.
    [[nodiscard]] std::vector<SettingDescriptor> Descriptors() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Row;
    struct Group;
    struct Section;

    void BuildFrame();
    void BuildSections();
    void BuildAppearanceRows(Section& section);
    void BuildServerRows();
    void ApplyIcons();
    Section& SectionFor(SettingsSection id);
    const Section* SectionFor(SettingsSection id) const;
    Group& GroupFor(SettingsSection section, const QString& title, bool serverGroup = false);
    Row& AddSwitchRow(Group& group, const SettingDescriptor& descriptor);
    Row& AddPlainRow(Group& group, const QString& label, const QString& description, QWidget* control,
        const QString& rowObjectName, std::vector<std::string> requiresAll);
    QFrame* MakeRowFrame(Row& row, const QString& objectName);
    void PlaceInGroup(Group& group, Row& row, bool beforeRisky);
    QLabel* AddBadge(Row& row, const QString& kind, const QString& text);
    Row* FindRow(const std::string& keyOrObjectName) const;
    Row* FindRowByWidget(const QObject* widget) const;

    void OnToggled(const std::string& key, bool checked);
    void AskToConfirm(const std::string& key, bool checked);
    void Commit(const std::string& key, bool checked);
    void FinishWrite(const std::string& key, quint64 serial, bool previousDisplay, std::optional<bool> written, const SettingWrite& result);
    void ResetRow(const std::string& key);
    void WriteProfileValue(const std::string& key, double value);
    void ShowInlineValue(Row& row, double value);
    void GuardWheel(QWidget* root);
    void CancelEdit(QWidget* editor);
    void ReadRows(const std::vector<Row*>& rows);
    void SetDisplay(Row& row, bool storedValue);
    [[nodiscard]] bool StoredValue(const std::string& key, bool& value) const;
    void EvaluateDependencies();
    void ApplyRowState(Row& row);
    void ShowTransientStatus(Row& row, const QString& text, const QString& tone);
    void RenderStatus(Row& row);
    void RenderTexts();
    void RenderRowTexts(Row& row);
    void UpdateRestartBanner();
    void ApplySearch(const QString& query);
    void LayoutDividers(Group& group);
    void ShowAllRows();
    [[nodiscard]] int HostedHits(const Section& section, const QStringList& terms) const;
    [[nodiscard]] QString Expand(const QString& text) const;
    [[nodiscard]] QString LabelFor(const std::string& key) const;
    void NotifyResize();

    SettingsBackend backend;
    SettingsHooks hooks;
    QString companionName;
    QString profileName;
    std::vector<std::unique_ptr<Section>> sections;
    std::vector<std::unique_ptr<Group>> groups;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::string> serverIds;
    std::function<void(QSize)> resizeHook;

    QWidget* navigation = nullptr;
    QLineEdit* search = nullptr;
    QListWidget* sectionList = nullptr;
    QStackedWidget* stack = nullptr;
    QFrame* restartBanner = nullptr;
    QLabel* restartLabel = nullptr;
    QPushButton* restartNow = nullptr;
    QLabel* noResults = nullptr;
    QButtonGroup* themeButtons = nullptr;
    QComboBox* textSizeCombo = nullptr;
    int restartDismissedAt = -1;
    bool updating = false;
    bool geometryRestored = false;
    quint64 statusSerial = 0;
    // Numbers every backend write, so the answer to one that was abandoned or replaced is ignored.
    quint64 writeSerial = 0;
};
}
