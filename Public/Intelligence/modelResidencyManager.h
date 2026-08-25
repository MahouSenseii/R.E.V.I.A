#pragma once

#include "Intelligence/intelligenceTypes.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace revia::intelligence
{

enum class ResidencyState
{
    Disabled,
    Cold,
    Loading,
    Warm,
    Failed
};

struct ModelResidency
{
    IntelligenceTier tier = IntelligenceTier::Main;
    std::string role;
    std::string model;
    std::string projector;
    std::string device;
    std::uint64_t artifactMiB = 0;
    ResidencyState state = ResidencyState::Cold;
    double loadMilliseconds = -1.0;
    std::uint64_t uses = 0;
    bool inferenceActive = false;
    std::string priority = "interactive";
    std::string detail;
};

// Thread-safe inventory for long-lived model roles. It does not guess live VRAM from
// file size; artifactMiB is explicitly an artifact estimate, while the resource monitor
// remains the source of measured process/GPU usage.
class ModelResidencyManager
{
public:
    void Register(ModelResidency model);
    void MarkLoading(IntelligenceTier tier);
    void MarkReady(IntelligenceTier tier, double loadMilliseconds, bool warm);
    void MarkFailed(IntelligenceTier tier, std::string reason);
    void BeginInference(IntelligenceTier tier, std::string priority);
    void EndInference(IntelligenceTier tier);
    [[nodiscard]] std::vector<ModelResidency> Snapshot() const;
    [[nodiscard]] std::string Summary() const;

private:
    ModelResidency* FindUnlocked(IntelligenceTier tier);
    mutable std::mutex mutex;
    std::vector<ModelResidency> models;
};

[[nodiscard]] std::string ToString(ResidencyState state);

} // namespace revia::intelligence
