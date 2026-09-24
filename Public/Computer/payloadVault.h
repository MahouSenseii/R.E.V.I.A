#pragma once

#include "Computer/computerSubgoal.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace revia::computer
{

// Where the user's exact words live while a decision is being made about them.
//
// The model is shown a reference and never the value. It decides that *something* goes
// in the message box; the runtime decides what that something is, and supplies the
// original at the moment the action is authorized. That ordering is what stops an
// approximation of a message being sent in place of the message.
//
// Scoped to one goal and cleared with it. A payload that outlived the task it was
// entered for would be user content sitting in memory with nothing left to justify it,
// and a reference that outlived its vault would be a dangling authorization -- so
// lookup fails closed rather than returning an empty string, which would silently type
// nothing into a field somebody is about to submit.
//
// Thread-safe because the decision path and the execution path are different threads,
// and a reference minted on one is redeemed on the other.
class PayloadVault
{
public:
    PayloadVault() = default;

    PayloadVault(const PayloadVault&) = delete;
    PayloadVault& operator=(const PayloadVault&) = delete;

    // Take custody of a value and hand back the only thing a model ever sees.
    //
    // `kind` is a coarse label -- "message", "filename", "search" -- that lets a policy
    // reason about where a value belongs without being shown it. The length is exposed
    // for the same reason: refusing an oversized entry needs the size, not the text.
    // `provenance` says where the value came from and travels with the reference. It
    // is a parameter rather than an inference because only the caller knows: the same
    // sentence is the user's own words when it was lifted from their request and a
    // draft when she composed it, and nothing about the string itself can tell them
    // apart.
    [[nodiscard]] PayloadReference Store(
        std::string value,
        std::string kind,
        ContentProvenance provenance = ContentProvenance::UserSupplied);

    // The original, or nothing.
    //
    // Nothing is the important case. A reference the vault does not recognise is a
    // reference a model invented, and answering it with an empty string would turn an
    // invented payload into a real keystroke sequence of length zero -- which in a
    // send-shaped workflow is an empty message actually sent.
    [[nodiscard]] std::optional<std::string> Redeem(const PayloadReference& reference) const;

    // Whether the reference names something real, without materialising it. Validation
    // asks this; only execution asks Redeem.
    [[nodiscard]] bool Holds(const PayloadReference& reference) const;

    // The runtime's own description of a held payload.
    //
    // A model naming a payload supplies an id and nothing else -- it has never seen the
    // value and cannot say how long it is. Taking the kind and length from the proposal
    // would mean a length check performed against a number the thing being checked
    // wrote, which is not a check. This returns the authoritative reference, and
    // validation substitutes it for the proposed one.
    [[nodiscard]] std::optional<PayloadReference> Describe(
        const PayloadReference& reference) const;

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
