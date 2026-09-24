#include "Computer/payloadVault.h"

#include <array>
#include <random>
#include <sstream>
#include <utility>

namespace revia::computer
{

namespace
{

// Random rather than sequential, and deliberately so.
//
// Every other id in this codebase is a timestamp and a counter, which is right for
// things whose ordering is informative. A payload reference is different: it is the
// only thing standing between a model and the user's exact words, and a counter would
// let a model that has seen one reference name the next one. It would still have to be
// a reference the *runtime* accepts for this subgoal, so this is a second lock rather
// than the only one -- but a second lock that costs one draw from a generator is worth
// having.
std::string NewPayloadId()
{
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::ostringstream stream;
    stream << "payload-" << std::hex << generator() << generator();
    return stream.str();
}

} // namespace

PayloadReference PayloadVault::Store(
    std::string value, std::string kind, const ContentProvenance provenance)
{
    PayloadReference reference;
    reference.kind = kind;
    reference.length = value.size();
    reference.provenance = provenance;
    reference.id = NewPayloadId();

    std::lock_guard lock(mutex);
    entries.emplace(reference.id, Entry{std::move(value), std::move(kind), provenance});
    return reference;
}

std::optional<std::string> PayloadVault::Redeem(const PayloadReference& reference) const
{
    if (reference.id.empty()) return std::nullopt;
    std::lock_guard lock(mutex);
    const auto found = entries.find(reference.id);
    // Fails closed. An unrecognised reference yields nothing at all rather than an
    // empty value, because "type nothing here" is a real action with real consequences
    // in a field that is about to be submitted.
    if (found == entries.end()) return std::nullopt;
    return found->second.value;
}

bool PayloadVault::Holds(const PayloadReference& reference) const
{
    if (reference.id.empty()) return false;
    std::lock_guard lock(mutex);
    return entries.find(reference.id) != entries.end();
}

std::optional<PayloadReference> PayloadVault::Describe(
    const PayloadReference& reference) const
{
    if (reference.id.empty()) return std::nullopt;
    std::lock_guard lock(mutex);
    const auto found = entries.find(reference.id);
    if (found == entries.end()) return std::nullopt;

    PayloadReference described;
    described.id = found->first;
    described.kind = found->second.kind;
    described.length = found->second.value.size();
    described.provenance = found->second.provenance;
    return described;
}

void PayloadVault::Clear()
{
    std::lock_guard lock(mutex);
    entries.clear();
}

std::size_t PayloadVault::Size() const
{
    std::lock_guard lock(mutex);
    return entries.size();
}

} // namespace revia::computer
