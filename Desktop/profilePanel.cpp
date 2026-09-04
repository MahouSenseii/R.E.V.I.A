#include "profilePanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

namespace
{
    constexpr auto WindowsVoiceLabel = "Windows voice (fallback)";

    QString DescribeVoice(const revia::runtime::ProfileSummary& profile)
    {
        if (profile.voicePresetId.empty())
        {
            return QStringLiteral("Windows voice");
        }
        return profile.voicePresetName.empty()
            ? QString::fromStdString(profile.voicePresetId)
            : QString::fromStdString(profile.voicePresetName);
    }

    QString ProfileLabel(const revia::runtime::ProfileSummary& profile)
    {
        return QString::fromStdString(profile.displayName) +
            " (" + QString::fromStdString(profile.id) + ")";
    }
}

ProfilePanel::ProfilePanel(
    revia::runtime::ReviaSession& inputSession,
    QWidget* parent)
    : QWidget(parent),
      session(inputSession)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* title = new QLabel("Profiles", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);

    auto* explanation = new QLabel(
        "A profile is who Revia is: her name, her personality, whether she remembers what "
        "you say, and which created voice speaks for her. Build voices in the Voice tab, "
        "then assign one here. Only profile files that parse are listed.", this);
    explanation->setWordWrap(true);
    explanation->setObjectName("secondaryText");
    layout->addWidget(explanation);

    activeBanner = new QLabel(this);
    activeBanner->setObjectName("profileActiveBanner");
    activeBanner->setWordWrap(true);
    layout->addWidget(activeBanner);

    auto* selectionRow = new QHBoxLayout();
    selectionRow->addWidget(new QLabel("Profile", this));
    profileCombo = new QComboBox(this);
    profileCombo->setMinimumWidth(220);
    profileCombo->setMaximumWidth(420);
    selectionRow->addWidget(profileCombo);
    activateButton = new QPushButton("Use this profile", this);
    selectionRow->addWidget(activateButton);
    newProfileButton = new QPushButton("New profile", this);
    selectionRow->addWidget(newProfileButton);
    selectionRow->addStretch();
    layout->addLayout(selectionRow);

    auto* identityGroup = new QGroupBox("Identity", this);
    auto* identityLayout = new QVBoxLayout(identityGroup);
    auto* identityForm = new QFormLayout();
    idInput = new QLineEdit(identityGroup);
    // Field width is a hint about expected content. A twenty-character id in a field
    // stretched across a 4K monitor reads as a mistake, not as spaciousness.
    idInput->setMaximumWidth(340);
    idInput->setPlaceholderText("revia");
    idInput->setToolTip(
        "The file name this profile is saved under, and the name /profile takes. "
        "Letters, numbers, dot, dash, and underscore.");
    identityForm->addRow("Profile id", idInput);
    displayNameInput = new QLineEdit(identityGroup);
    displayNameInput->setMaximumWidth(420);
    displayNameInput->setPlaceholderText("Revia");
    identityForm->addRow("Display name", displayNameInput);
    descriptionInput = new QLineEdit(identityGroup);
    descriptionInput->setMaximumWidth(760);
    descriptionInput->setPlaceholderText(
        "One line so you can tell this profile apart from the others.");
    identityForm->addRow("Description", descriptionInput);
    identityLayout->addLayout(identityForm);

    identityLayout->addWidget(new QLabel("Personality and instructions", identityGroup));
    systemPromptInput = new QPlainTextEdit(identityGroup);
    systemPromptInput->setMinimumHeight(200);
    systemPromptInput->setPlaceholderText(
        "You are a helpful local AI assistant. Keep responses clear, useful, and concise.");
    identityLayout->addWidget(systemPromptInput);

    memoryCheck = new QCheckBox("Remember what is said to this profile", identityGroup);
    memoryCheck->setToolTip(
        "Long-term memory is wired up during startup, so a change here applies at the "
        "next start.");
    identityLayout->addWidget(memoryCheck);

    // How complete an answer this profile owes. Deliberately worded around
    // completeness rather than temperament: how she says a thing is her personality's
    // business, and describing these as nicer or more difficult would make this
    // control quietly edit her character.
    answerStyleCombo = new QComboBox(identityGroup);
    answerStyleCombo->addItem("Reliable",
        static_cast<int>(AnswerObligationMode::Reliable));
    answerStyleCombo->addItem("Balanced",
        static_cast<int>(AnswerObligationMode::Balanced));
    answerStyleCombo->addItem("Character First",
        static_cast<int>(AnswerObligationMode::CharacterFirst));
    answerStyleCombo->setToolTip(
        "Answer style. Reliable usually gives the substantive answer, in her own "
        "voice. Balanced usually answers, and her current state may affect how "
        "complete it is. Character First lets her personality take priority over "
        "completing the answer in ordinary conversation. This does not change who "
        "she is, only how much of an answer she owes.");
    auto* answerStyleRow = new QHBoxLayout();
    answerStyleRow->addWidget(new QLabel("Answer style", identityGroup));
    answerStyleRow->addWidget(answerStyleCombo, 1);
    identityLayout->addLayout(answerStyleRow);

    auto* samplingRow = new QHBoxLayout();
    temperatureCheck = new QCheckBox("Override temperature", identityGroup);
    temperatureSpin = new QDoubleSpinBox(identityGroup);
    temperatureSpin->setRange(0.0, 2.0);
    temperatureSpin->setSingleStep(0.05);
    temperatureSpin->setDecimals(2);
    temperatureSpin->setValue(0.7);
    // Without a floor the spin buttons are clipped to a pair of slivers, and the suffix
    // on the reply limit pushes its own value out of view entirely.
    temperatureSpin->setMinimumWidth(110);
    maxTokensCheck = new QCheckBox("Override reply limit", identityGroup);
    maxTokensSpin = new QSpinBox(identityGroup);
    maxTokensSpin->setRange(1, 32768);
    maxTokensSpin->setValue(512);
    maxTokensSpin->setSuffix(" tokens");
    maxTokensSpin->setMinimumWidth(150);
    samplingRow->addWidget(temperatureCheck);
    samplingRow->addWidget(temperatureSpin);
    samplingRow->addSpacing(18);
    samplingRow->addWidget(maxTokensCheck);
    samplingRow->addWidget(maxTokensSpin);
    samplingRow->addStretch();
    identityLayout->addLayout(samplingRow);
    layout->addWidget(identityGroup);

    auto* voiceGroup = new QGroupBox("Voice", this);
    auto* voiceLayout = new QVBoxLayout(voiceGroup);
    auto* voiceHint = new QLabel(
        "The voice this profile speaks with. Assigning takes effect immediately; a Qwen "
        "voice loads onto its planned device at the next start.", voiceGroup);
    voiceHint->setWordWrap(true);
    voiceHint->setObjectName("secondaryText");
    voiceLayout->addWidget(voiceHint);
    auto* voiceRow = new QHBoxLayout();
    voiceCombo = new QComboBox(voiceGroup);
    voiceCombo->setMinimumWidth(220);
    voiceRow->addWidget(voiceCombo);
    assignVoiceButton = new QPushButton("Assign voice", voiceGroup);
    voiceRow->addWidget(assignVoiceButton);
    windowsVoiceButton = new QPushButton("Use Windows voice", voiceGroup);
    voiceRow->addWidget(windowsVoiceButton);
    voiceRow->addStretch();
    voiceLayout->addLayout(voiceRow);
    layout->addWidget(voiceGroup);

    auto* buttonRow = new QHBoxLayout();
    saveButton = new QPushButton("Save profile", this);
    buttonRow->addWidget(saveButton);
    revertButton = new QPushButton("Discard changes", this);
    buttonRow->addWidget(revertButton);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("profileStatus");
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);
    layout->addStretch();

    connect(profileCombo, &QComboBox::currentIndexChanged, this, [this](int)
    {
        if (refreshing)
        {
            return;
        }
        draftProfile = false;
        LoadSelectedProfile();
    });
    connect(newProfileButton, &QPushButton::clicked, this, [this]() { BeginNewProfile(); });
    connect(activateButton, &QPushButton::clicked, this, [this]() { Activate(); });
    connect(saveButton, &QPushButton::clicked, this, [this]() { Save(); });
    connect(revertButton, &QPushButton::clicked, this, [this]()
    {
        draftProfile = false;
        Refresh();
        SetStatus("Reloaded this profile from disk.");
    });
    connect(assignVoiceButton, &QPushButton::clicked, this, [this]() { AssignVoice(false); });
    connect(windowsVoiceButton, &QPushButton::clicked, this, [this]() { AssignVoice(true); });
    connect(temperatureCheck, &QCheckBox::toggled, temperatureSpin, &QWidget::setEnabled);
    connect(maxTokensCheck, &QCheckBox::toggled, maxTokensSpin, &QWidget::setEnabled);
    temperatureSpin->setEnabled(false);
    maxTokensSpin->setEnabled(false);

    SetStatus("Profiles are ready.");
}

const revia::runtime::ProfileSummary* ProfilePanel::FindProfile(
    const std::string& profileId) const
{
    for (const revia::runtime::ProfileSummary& candidate : snapshot.profiles)
    {
        if (candidate.id == profileId)
        {
            return &candidate;
        }
    }
    return nullptr;
}

void ProfilePanel::Refresh()
{
    // A half-written new profile is not on disk and would not survive the rebuild, so the
    // selection is only restored for profiles that exist.
    const QString previous = draftProfile
        ? QString()
        : profileCombo->currentData().toString();
    snapshot = session.ProfileStudio();

    {
        refreshing = true;
        const QSignalBlocker blocker(profileCombo);
        profileCombo->clear();
        for (const revia::runtime::ProfileSummary& profile : snapshot.profiles)
        {
            profileCombo->addItem(
                ProfileLabel(profile), QString::fromStdString(profile.id));
        }
        int index = profileCombo->findData(previous.isEmpty()
            ? QString::fromStdString(snapshot.activeProfileId)
            : previous);
        if (index < 0 && profileCombo->count() > 0)
        {
            index = 0;
        }
        profileCombo->setCurrentIndex(index);
        refreshing = false;
    }

    {
        const QSignalBlocker blocker(voiceCombo);
        const QString previousVoice = voiceCombo->currentData().toString();
        voiceCombo->clear();
        voiceCombo->addItem(WindowsVoiceLabel, QString());
        for (const revia::speech::VoicePreset& preset : snapshot.voices)
        {
            voiceCombo->addItem(
                QString::fromStdString(preset.name),
                QString::fromStdString(preset.id));
        }
        const int voiceIndex = voiceCombo->findData(previousVoice);
        voiceCombo->setCurrentIndex(voiceIndex >= 0 ? voiceIndex : 0);
    }

    UpdateBanner();
    if (draftProfile)
    {
        ApplyDraftState();
    }
    else
    {
        LoadSelectedProfile();
    }
}

void ProfilePanel::UpdateBanner()
{
    // Before startup finishes, settings.json has not been read and the session is still
    // holding compiled defaults. Naming one of them as the profile in use would be a
    // guess, and this banner exists precisely so the answer is not a guess.
    if (!session.IsStarted())
    {
        activeBanner->setText(
            "Revia is still starting. The profile in use appears once she is online.");
        return;
    }
    if (snapshot.activeProfileId.empty())
    {
        activeBanner->setText("No profile is loaded yet.");
        return;
    }
    QString voice = QStringLiteral("Windows voice");
    if (const revia::runtime::ProfileSummary* active = FindProfile(snapshot.activeProfileId))
    {
        voice = DescribeVoice(*active);
    }
    activeBanner->setText(
        "Now using " + QString::fromStdString(snapshot.activeDisplayName) +
        "   \xE2\x80\xA2   id " + QString::fromStdString(snapshot.activeProfileId) +
        "   \xE2\x80\xA2   voice " + voice);
}

void ProfilePanel::LoadSelectedProfile()
{
    const std::string selected = profileCombo->currentData().toString().toStdString();
    const revia::runtime::ProfileSummary* profile = FindProfile(selected);
    if (profile == nullptr)
    {
        ApplyDraftState();
        return;
    }
    idInput->setText(QString::fromStdString(profile->id));
    displayNameInput->setText(QString::fromStdString(profile->displayName));
    descriptionInput->setText(QString::fromStdString(profile->description));
    systemPromptInput->setPlainText(QString::fromStdString(profile->systemPrompt));
    memoryCheck->setChecked(profile->memoryEnabled);
    answerStyleCombo->setCurrentIndex(
        answerStyleCombo->findData(static_cast<int>(profile->answerObligation)));
    temperatureCheck->setChecked(profile->hasTemperatureOverride);
    temperatureSpin->setValue(profile->temperature);
    maxTokensCheck->setChecked(profile->hasMaxTokensOverride);
    maxTokensSpin->setValue(profile->maxTokens);
    {
        const QSignalBlocker blocker(voiceCombo);
        const int voiceIndex =
            voiceCombo->findData(QString::fromStdString(profile->voicePresetId));
        voiceCombo->setCurrentIndex(voiceIndex >= 0 ? voiceIndex : 0);
    }
    ApplyDraftState();
}

void ProfilePanel::BeginNewProfile()
{
    draftProfile = true;
    idInput->clear();
    displayNameInput->clear();
    descriptionInput->clear();
    systemPromptInput->clear();
    memoryCheck->setChecked(true);
    answerStyleCombo->setCurrentIndex(
        answerStyleCombo->findData(static_cast<int>(AnswerObligationMode::Balanced)));
    temperatureCheck->setChecked(false);
    temperatureSpin->setValue(0.7);
    maxTokensCheck->setChecked(false);
    maxTokensSpin->setValue(512);
    {
        const QSignalBlocker blocker(voiceCombo);
        voiceCombo->setCurrentIndex(0);
    }
    ApplyDraftState();
    idInput->setFocus();
    SetStatus("Give the new profile an id, a display name, and a personality, then save it.");
}

void ProfilePanel::ApplyDraftState()
{
    // The id names the file. Renaming an existing profile through this field would leave
    // the original behind and orphan its voice assignment, so it is fixed once saved.
    idInput->setReadOnly(!draftProfile);
    const bool onDisk = !draftProfile && !idInput->text().trimmed().isEmpty();
    // Switching profiles and assigning a voice both reach into a live runtime. Editing
    // and saving a profile is only a file write, so that stays available while Revia
    // starts up.
    const bool live = onDisk && session.IsStarted();
    voiceCombo->setEnabled(live);
    assignVoiceButton->setEnabled(live);
    windowsVoiceButton->setEnabled(live);
    revertButton->setEnabled(!snapshot.profiles.empty());
    const bool isActive =
        live && idInput->text().toStdString() == snapshot.activeProfileId;
    activateButton->setText(isActive ? "Already in use" : "Use this profile");
    activateButton->setEnabled(live && !isActive);
}

void ProfilePanel::Save()
{
    revia::runtime::ProfileSummary definition;
    definition.id = idInput->text().trimmed().toStdString();
    definition.displayName = displayNameInput->text().trimmed().toStdString();
    definition.description = descriptionInput->text().trimmed().toStdString();
    definition.systemPrompt = systemPromptInput->toPlainText().trimmed().toStdString();
    definition.memoryEnabled = memoryCheck->isChecked();
    definition.answerObligation = static_cast<AnswerObligationMode>(
        answerStyleCombo->currentData().toInt());
    definition.hasTemperatureOverride = temperatureCheck->isChecked();
    definition.temperature = static_cast<float>(temperatureSpin->value());
    definition.hasMaxTokensOverride = maxTokensCheck->isChecked();
    definition.maxTokens = maxTokensSpin->value();

    if (draftProfile && FindProfile(definition.id) != nullptr)
    {
        SetStatus(
            "A profile with id " + QString::fromStdString(definition.id) +
            " already exists. Choose another id, or select it above to edit it.", true);
        return;
    }

    const revia::runtime::ProfileOperationResult result = session.SaveProfile(definition);
    if (!result.succeeded)
    {
        SetStatus(QString::fromStdString(result.message), true);
        return;
    }
    const QString savedId = QString::fromStdString(definition.id);
    draftProfile = false;
    Refresh();
    {
        refreshing = true;
        const QSignalBlocker blocker(profileCombo);
        const int index = profileCombo->findData(savedId);
        if (index >= 0)
        {
            profileCombo->setCurrentIndex(index);
        }
        refreshing = false;
    }
    LoadSelectedProfile();
    SetStatus(QString::fromStdString(result.message));
}

void ProfilePanel::Activate()
{
    const std::string selected = profileCombo->currentData().toString().toStdString();
    if (selected.empty())
    {
        SetStatus("Save this profile before switching to it.", true);
        return;
    }
    const revia::runtime::ProfileOperationResult result = session.ActivateProfile(selected);
    if (!result.succeeded)
    {
        SetStatus(QString::fromStdString(result.message), true);
        return;
    }
    Refresh();
    SetStatus(QString::fromStdString(result.message));
}

void ProfilePanel::AssignVoice(const bool useWindowsFallback)
{
    const std::string profileId = profileCombo->currentData().toString().toStdString();
    if (profileId.empty())
    {
        SetStatus("Save this profile before assigning it a voice.", true);
        return;
    }
    const std::string presetId = useWindowsFallback
        ? std::string()
        : voiceCombo->currentData().toString().toStdString();
    if (!useWindowsFallback && presetId.empty())
    {
        SetStatus("Pick a created voice first, or choose Use Windows voice.", true);
        return;
    }
    const revia::speech::VoiceOperationResult result =
        session.AssignVoice(profileId, presetId);
    Refresh();
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
}

void ProfilePanel::SetStatus(const QString& text, const bool error)
{
    statusLabel->setText(text);
    statusLabel->setProperty("error", error);
    statusLabel->style()->unpolish(statusLabel);
    statusLabel->style()->polish(statusLabel);
}
