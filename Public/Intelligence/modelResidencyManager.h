#pragma once

#include "Intelligence/intelligenceTypes.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace revia::intelligence
{

// Where a model role actually stands.
//
// Unloaded and Failed are the distinction this enum exists to keep. A model that was
// put away on purpose and a model that tried to start and could not are both "not
// available right now", and treating them the same is how a deliberately evicted worker
// gets reported as broken -- and, worse, how routing stops trying to bring it back.
//
// Cold means the process is up and has not been warmed; Unloaded means there is no
// process. Both are enabled and both are reachable by loading; only one costs memory.
enum class ResidencyState
{
    // Not configured, or its artifacts are absent. Nothing to load.
    Disabled,
    // Enabled and not resident. A request can bring it back; it holds no memory.
    Unloaded,
    // Resident but not warmed.
    Cold,
    Loading,
    Warm,
    // Tried and could not. Distinct from Unloaded, because retrying this is a different
    // decision from loading something that was simply put away.
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
    // Put away on purpose. Clears active inference and the load time, because neither
    // describes anything that currently exists, and leaves the role enabled.
    void MarkUnloaded(IntelligenceTier tier, std::string reason);
    // True while a role holds memory: loading, resident, or in use. What an idle
    // sweeper asks before deciding there is anything to release.
    [[nodiscard]] bool IsResident(IntelligenceTier tier) const;
    [[nodiscard]] bool IsInferenceActive(IntelligenceTier tier) const;
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
