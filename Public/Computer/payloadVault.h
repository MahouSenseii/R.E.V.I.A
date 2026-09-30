#pragma once

#include "Computer/computerSubgoal.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace revia::computer
{

// Thread-safe custody of exact content for one goal; models receive references only.
// Clear with the goal and fail closed on unknown references.
class PayloadVault
{
public:
    PayloadVault() = default;

    PayloadVault(const PayloadVault&) = delete;
    PayloadVault& operator=(const PayloadVault&) = delete;

    // Exposes kind and length without content. The caller supplies provenance;
    // identical text may be user-authored or a generated draft.
    [[nodiscard]] PayloadReference Store(std::string value,
        std::string kind, ContentProvenance provenance = ContentProvenance::UserSupplied);

    // Unknown references return no value, never empty text to submit.
    [[nodiscard]] std::optional<std::string> Redeem(const PayloadReference& reference) const;

    // Whether the reference names something real, without materialising it. Validation
    // asks this; only execution asks Redeem.
    [[nodiscard]] bool Holds(const PayloadReference& reference) const;

    // Returns the vault's authoritative metadata, ignoring fields claimed by the caller.
    [[nodiscard]] std::optional<PayloadReference> Describe(const PayloadReference& reference) const;

    // Forget everything. Called when the goal that owned these ends, however it ends.
    void Clear();

    [[nodiscard]] std::size_t Size() const;

private:
    mutable std::mutex mutex;
    struct Entry
    {
        std::string value;
        std::string kind;
        ContentProvenance provenance = ContentProvenance::None;
    };
    std::unordered_map<std::string, Entry> entries;
};

} // namespace revia::computer
