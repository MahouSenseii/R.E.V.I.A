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
        case ResidencyState::Unloaded: return "Unloaded";
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

void ModelResidencyManager::MarkUnloaded(
    const IntelligenceTier tier,
    std::string reason)
{
    std::lock_guard lock(mutex);
    if (ModelResidency* model = FindUnlocked(tier))
    {
        // Disabled outranks this. A role with no artifacts was never resident and did
        // not become loadable by being swept.
        if (model->state == ResidencyState::Disabled) return;
        model->state = ResidencyState::Unloaded;
        model->inferenceActive = false;
        // Neither describes anything that exists now. A stale load time on an unloaded
        // role would read as a measurement of the next load, which it is not.
        model->loadMilliseconds = -1.0;
        model->detail = std::move(reason);
    }
}

bool ModelResidencyManager::IsResident(const IntelligenceTier tier) const
{
    std::lock_guard lock(mutex);
    const auto found = std::find_if(models.begin(), models.end(),
        [tier](const ModelResidency& model) { return model.tier == tier; });
    if (found == models.end()) return false;
    // Loading counts. A load in flight is already holding memory, and a sweeper that
    // thought otherwise would race the thing it is trying not to evict.
    return found->state == ResidencyState::Loading ||
        found->state == ResidencyState::Cold ||
        found->state == ResidencyState::Warm;
}

bool ModelResidencyManager::IsInferenceActive(const IntelligenceTier tier) const
{
    std::lock_guard lock(mutex);
    const auto found = std::find_if(models.begin(), models.end(),
        [tier](const ModelResidency& model) { return model.tier == tier; });
    return found != models.end() && found->inferenceActive;
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
