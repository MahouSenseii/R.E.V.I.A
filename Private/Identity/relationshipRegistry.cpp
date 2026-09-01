#include "Identity/relationshipRegistry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace revia::identity
{

namespace
{
    std::string Timestamp()
    {
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm parts{};
#ifdef _WIN32
        gmtime_s(&parts, &now);
#else
        gmtime_r(&now, &parts);
#endif
        std::ostringstream stamp;
        stamp << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
        return stamp.str();
    }

    // Entity ids reach a file name only indirectly, but they do key a persisted map, so
    // they are normalised to something stable and printable rather than trusting an
    // adapter to supply a sane author string.
    std::string Sanitize(const std::string& value)
    {
        std::string output;
        output.reserve(value.size());
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) != 0)
            {
                output.push_back(static_cast<char>(std::tolower(character)));
            }
            else if (!output.empty() && output.back() != '-')
            {
                output.push_back('-');
            }
        }
        while (!output.empty() && output.back() == '-')
        {
            output.pop_back();
        }
        return output.substr(0, 64);
    }
}

std::string LocalUserEntityId()
{
    return "local:user";
}

std::string AdapterEntityId(const std::string& source, const std::string& author)
{
    const std::string cleanSource = Sanitize(source);
    const std::string cleanAuthor = Sanitize(author);
    if (cleanAuthor.empty())
    {
        return "adapter:" + (cleanSource.empty() ? "unknown" : cleanSource) + ":anonymous";
    }
    return "adapter:" + (cleanSource.empty() ? "unknown" : cleanSource) + ":" + cleanAuthor;
}

RelationshipRegistry::RelationshipRegistry(std::filesystem::path path)
    : store(std::move(path))
{
}

bool RelationshipRegistry::Load(std::string& outError)
{
    std::lock_guard lock(mutex);
    if (!store.Load(snapshot, outError))
    {
        return false;
    }
    preferences.Replace(snapshot.preferences);
    return true;
}

bool RelationshipRegistry::Save(std::string& outError) const
{
    std::lock_guard lock(mutex);
    return store.Save(snapshot, outError);
}

Preference RelationshipRegistry::ReinforcePreference(
    const std::string& subject, const bool positive, const PreferenceSource source)
{
    std::lock_guard lock(mutex);
    const Preference updated =
        preferences.Reinforce(subject, positive, source, Timestamp());
    // The snapshot is what persistence writes, so it has to follow every change rather
    // than only the ones that happen to precede a save.
    snapshot.preferences = preferences.All();
    return updated;
}

void RelationshipRegistry::SeedPreferences(
    const std::vector<std::pair<std::string, float>>& declared)
{
    std::lock_guard lock(mutex);
    bool inserted = false;
    for (const auto& [subject, strength] : declared)
    {
        inserted = preferences.SeedFromProfile(subject, strength) || inserted;
    }
    if (inserted)
    {
        snapshot.preferences = preferences.All();
    }
}

std::vector<Preference> RelationshipRegistry::Preferences() const
{
    std::lock_guard lock(mutex);
    return preferences.All();
}

std::vector<Preference> RelationshipRegistry::StrongestPreferences(
    const std::size_t limit) const
{
    std::lock_guard lock(mutex);
    return preferences.Strongest(limit);
}

RelationshipState RelationshipRegistry::Get(const std::string& entityId)
{
    std::lock_guard lock(mutex);
    const auto found = snapshot.relationships.find(entityId);
    if (found != snapshot.relationships.end())
    {
        return found->second;
    }
    // First contact. Neutral, not warm: being met is not the same as being liked.
    RelationshipState fresh;
    fresh.entityId = entityId;
    snapshot.relationships.emplace(entityId, fresh);
    return fresh;
}

std::optional<RelationshipState> RelationshipRegistry::Find(
    const std::string& entityId) const
{
    std::lock_guard lock(mutex);
    const auto found = snapshot.relationships.find(entityId);
    if (found == snapshot.relationships.end())
    {
        return std::nullopt;
    }
    return found->second;
}

std::vector<RelationshipState> RelationshipRegistry::All() const
{
    std::lock_guard lock(mutex);
    std::vector<RelationshipState> everyone;
    everyone.reserve(snapshot.relationships.size());
    for (const auto& [entityId, relationship] : snapshot.relationships)
    {
        everyone.push_back(relationship);
    }
    return everyone;
}

RelationshipState RelationshipRegistry::Apply(const RelationshipEvent& event)
{
    if (event.entityId.empty())
    {
        return {};
    }
    std::lock_guard lock(mutex);
    auto found = snapshot.relationships.find(event.entityId);
    if (found == snapshot.relationships.end())
    {
        RelationshipState fresh;
        fresh.entityId = event.entityId;
        found = snapshot.relationships.emplace(event.entityId, fresh).first;
    }
    found->second = ApplyRelationshipEvent(found->second, event);
    return found->second;
}

void RelationshipRegistry::SettleAll()
{
    std::lock_guard lock(mutex);
    for (auto& [entityId, relationship] : snapshot.relationships)
    {
        relationship = SettleRelationship(relationship);
    }
}

void RelationshipRegistry::SetDisplayName(
    const std::string& entityId,
    const std::string& displayName)
{
    if (entityId.empty() || displayName.empty())
    {
        return;
    }
    std::lock_guard lock(mutex);
    auto found = snapshot.relationships.find(entityId);
    if (found == snapshot.relationships.end())
    {
        RelationshipState fresh;
        fresh.entityId = entityId;
        found = snapshot.relationships.emplace(entityId, fresh).first;
    }
    found->second.displayName = displayName;
}

std::string RelationshipRegistry::NamedLocalEntityId(const std::string& name)
{
    const std::string slug = Sanitize(name);
    return slug.empty() ? LocalUserEntityId() : "local:" + slug;
}

std::string RelationshipRegistry::ResolveNamedLocalSpeaker(const std::string& name)
{
    const std::string target = NamedLocalEntityId(name);
    if (target == LocalUserEntityId())
    {
        return target;
    }

    std::lock_guard lock(mutex);
    if (snapshot.relationships.count(target) != 0)
    {
        // Someone she already knows has come back.
        snapshot.relationships[target].displayName = name;
        return target;
    }

    // Has any local speaker been named yet? If not, this is the first introduction and
    // the anonymous history belongs to them.
    const bool anyNamedLocal = std::any_of(
        snapshot.relationships.begin(), snapshot.relationships.end(),
        [](const auto& entry)
        {
            return entry.first.rfind("local:", 0) == 0 &&
                entry.first != LocalUserEntityId();
        });

    const auto anonymous = snapshot.relationships.find(LocalUserEntityId());
    if (!anyNamedLocal && anonymous != snapshot.relationships.end())
    {
        RelationshipState adopted = anonymous->second;
        adopted.entityId = target;
        adopted.displayName = name;
        snapshot.relationships.erase(anonymous);
        snapshot.relationships.emplace(target, adopted);
        return target;
    }

    // A second person at the same keyboard. Their own relationship, starting neutral --
    // inheriting someone else's trust because they share a chair would be absurd.
    RelationshipState fresh;
    fresh.entityId = target;
    fresh.displayName = name;
    snapshot.relationships.emplace(target, fresh);
    return target;
}

DevelopmentState RelationshipRegistry::Development() const
{
    std::lock_guard lock(mutex);
    return snapshot.development;
}

void RelationshipRegistry::SetDevelopment(const DevelopmentState& development)
{
    std::lock_guard lock(mutex);
    snapshot.development = development;
}

void RelationshipRegistry::SetDevelopmentBaseline(const TraitVector& baseline)
{
    std::lock_guard lock(mutex);
    snapshot.development.base = baseline;
}

void RelationshipRegistry::RecordDevelopmentChange(const DevelopmentChange& change)
{
    std::lock_guard lock(mutex);
    snapshot.developmentHistory.push_back(change);
    // Bounded. The recent explanation is what is useful; an unbounded ledger would grow
    // without limit and slow every save down for history nobody reads.
    constexpr std::size_t maximumHistory = 200;
    if (snapshot.developmentHistory.size() > maximumHistory)
    {
        snapshot.developmentHistory.erase(
            snapshot.developmentHistory.begin(),
            snapshot.developmentHistory.begin() +
                static_cast<std::ptrdiff_t>(
                    snapshot.developmentHistory.size() - maximumHistory));
    }
}

std::vector<DevelopmentChange> RelationshipRegistry::DevelopmentHistory() const
{
    std::lock_guard lock(mutex);
    return snapshot.developmentHistory;
}

emotion::MoodState RelationshipRegistry::Mood() const
{
    std::lock_guard lock(mutex);
    return snapshot.mood;
}

void RelationshipRegistry::SetMood(const emotion::MoodState& mood)
{
    std::lock_guard lock(mutex);
    snapshot.mood = mood;
}

std::size_t RelationshipRegistry::Count() const
{
    std::lock_guard lock(mutex);
    return snapshot.relationships.size();
}

} // namespace revia::identity
