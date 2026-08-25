#include "Intelligence/modelResidencyManager.h"

#include <algorithm>
#include <sstream>

namespace revia::intelligence
{

std::string ToString(const ResidencyState state)
{
    switch (state)
    {
        case ResidencyState::Disabled: return "Disabled";
        case ResidencyState::Cold: return "Cold";
        case ResidencyState::Loading: return "Loading";
        case ResidencyState::Warm: return "Warm";
        case ResidencyState::Failed: return "Failed";
        default: return "Cold";
    }
}

ModelResidency* ModelResidencyManager::FindUnlocked(const IntelligenceTier tier)
{
    const auto found = std::find_if(models.begin(), models.end(),
        [tier](const ModelResidency& model) { return model.tier == tier; });
    return found == models.end() ? nullptr : &*found;
}

void ModelResidencyManager::Register(ModelResidency model)
{
    std::lock_guard lock(mutex);
    if (ModelResidency* existing = FindUnlocked(model.tier)) *existing = std::move(model);
    else models.push_back(std::move(model));
}

void ModelResidencyManager::MarkLoading(const IntelligenceTier tier)
{
    std::lock_guard lock(mutex);
    if (ModelResidency* model = FindUnlocked(tier))
    {
        model->state = ResidencyState::Loading;
        model->detail = "The model process is loading.";
    }
}

void ModelResidencyManager::MarkReady(
    const IntelligenceTier tier,
    const double loadMilliseconds,
    const bool warm)
{
    std::lock_guard lock(mutex);
    if (ModelResidency* model = FindUnlocked(tier))
    {
        model->state = warm ? ResidencyState::Warm : ResidencyState::Cold;
        model->loadMilliseconds = loadMilliseconds;
        model->detail = warm ? "Loaded and warmed." : "Available but not warmed.";
    }
}

void ModelResidencyManager::MarkFailed(
    const IntelligenceTier tier,
    std::string reason)
{
    std::lock_guard lock(mutex);
    if (ModelResidency* model = FindUnlocked(tier))
    {
        model->state = ResidencyState::Failed;
        model->inferenceActive = false;
        model->detail = std::move(reason);
    }
}

void ModelResidencyManager::BeginInference(
    const IntelligenceTier tier,
    std::string priority)
{
    std::lock_guard lock(mutex);
    if (ModelResidency* model = FindUnlocked(tier))
    {
        model->inferenceActive = true;
        model->priority = std::move(priority);
        ++model->uses;
    }
}

void ModelResidencyManager::EndInference(const IntelligenceTier tier)
{
    std::lock_guard lock(mutex);
    if (ModelResidency* model = FindUnlocked(tier)) model->inferenceActive = false;
}

std::vector<ModelResidency> ModelResidencyManager::Snapshot() const
{
    std::lock_guard lock(mutex);
    return models;
}

std::string ModelResidencyManager::Summary() const
{
    const std::vector<ModelResidency> snapshot = Snapshot();
    std::ostringstream output;
    for (std::size_t index = 0; index < snapshot.size(); ++index)
    {
        const ModelResidency& model = snapshot[index];
        if (index > 0) output << '\n';
        output << model.role << ": " << ToString(model.state) << " / "
            << model.model << " / " << model.device << " / " << model.artifactMiB
            << " MiB artifact / " << model.uses << " uses";
        if (!model.detail.empty()) output << " — " << model.detail;
    }
    return output.str();
}

} // namespace revia::intelligence
