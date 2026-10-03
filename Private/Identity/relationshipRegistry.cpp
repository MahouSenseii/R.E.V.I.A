#include "Identity/relationshipRegistry.h"
#include "Actions/actionTypes.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cmath>
#include <limits>
#include <iomanip>
#include <sstream>
#include <utility>

namespace revia::identity
{

namespace
{
constexpr std::size_t MaximumEvidence = 256;
constexpr std::size_t MaximumAliases = 512;
constexpr std::size_t MaximumConsents = 128;

bool ValidText(const std::string& value, const std::size_t limit)
{
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(), [](const unsigned char byte) { return byte < 32 || byte == 127; });
}

bool ValidFeatures(const speech::SpeakerFeatures& features)
{
    double magnitude = 0.0;
    for (const float value : features.values)
    {
        if (!std::isfinite(value) || std::abs(value) > 1.0F)
            return false;
        magnitude += static_cast<double>(value) * value;
    }
    return features.version == speech::SpeakerFeatureVersion && magnitude > 0.0;
}

RelationshipState ApplyRecord(RelationshipState state, const RelationshipEvidenceRecord& record)
{
    return record.kind == RelationshipEvidenceKind::Settling ? SettleRelationship(state)
                                                             : ApplyRelationshipEvent(state, record.event, {}, record.observedAt);
}
std::string Timestamp()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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

RelationshipRegistry::RelationshipRegistry(std::filesystem::path path) : store(std::move(path))
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
    if (snapshot.relationshipEvidence.empty() && snapshot.relationshipEvidenceBase.empty())
        snapshot.relationshipEvidenceBase = snapshot.relationships;
    frictionUpdatedAt.clear();
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [entityId, relationship] : snapshot.relationships)
    {
        (void)relationship;
        frictionUpdatedAt.emplace(entityId, now);
    }
    return true;
}

bool RelationshipRegistry::Save(std::string& outError) const
{
    std::lock_guard lock(mutex);
    return store.Save(snapshot, outError);
}

Preference RelationshipRegistry::ReinforcePreference(const std::string& subject, const bool positive, const PreferenceSource source)
{
    std::lock_guard lock(mutex);
    const Preference updated = preferences.Reinforce(subject, positive, source, Timestamp());
    // The snapshot is what persistence writes, so it has to follow every change rather
    // than only the ones that happen to precede a save.
    snapshot.preferences = preferences.All();
    return updated;
}

void RelationshipRegistry::SeedPreferences(const std::vector<std::pair<std::string, float>>& declared)
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

std::vector<Preference> RelationshipRegistry::StrongestPreferences(const std::size_t limit) const
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

std::optional<RelationshipState> RelationshipRegistry::Find(const std::string& entityId) const
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
    if (!ValidText(event.entityId, 160) || (!event.evidenceId.empty() && !ValidText(event.evidenceId, 160)))
    {
        return {};
    }
    if (event.description.size() > 512)
        return {};
    for (const float value : {event.positiveInteraction, event.negativeInteraction, event.trustEvidence, event.disrespectEvidence,
             event.cooperation, event.conflict, event.importance, event.confidence})
    {
        if (!std::isfinite(value) || std::abs(value) > 1.0F)
            return {};
    }
    std::lock_guard lock(mutex);
    if (!event.evidenceId.empty())
    {
        const auto prior = std::find_if(snapshot.relationshipEvidence.begin(), snapshot.relationshipEvidence.end(),
            [&event](const auto& record) { return record.event.evidenceId == event.evidenceId; });
        if (prior != snapshot.relationshipEvidence.end())
            return snapshot.relationships.at(prior->event.entityId);
    }
    auto found = snapshot.relationships.find(event.entityId);
    if (found == snapshot.relationships.end())
    {
        RelationshipState fresh;
        fresh.entityId = event.entityId;
        found = snapshot.relationships.emplace(event.entityId, fresh).first;
    }
    snapshot.relationshipEvidenceBase.try_emplace(event.entityId, found->second);
    // The registry supplies the clock so ApplyRelationshipEvent stays pure. Seconds,
    // matching the stamps the memory block already describes in words.
    RelationshipEvidenceRecord record;
    record.event = event;
    if (record.event.evidenceId.empty())
        record.event.evidenceId = actions::NewActionId();
    record.originalEntityId = event.entityId;
    record.observedAt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    found->second = ApplyRecord(found->second, record);
    snapshot.relationshipEvidence.push_back(std::move(record));
    CompactEvidenceLocked();
    frictionUpdatedAt[event.entityId] = std::chrono::steady_clock::now();
    return found->second;
}

void RelationshipRegistry::SettleAll(const std::chrono::steady_clock::time_point now, const std::chrono::milliseconds quietInterval)
{
    std::lock_guard lock(mutex);
    for (auto& [entityId, relationship] : snapshot.relationships)
    {
        auto [clock, inserted] = frictionUpdatedAt.try_emplace(entityId, now);
        if (inserted || now < clock->second || now - clock->second < quietInterval)
            continue;
        snapshot.relationshipEvidenceBase.try_emplace(entityId, relationship);
        RelationshipEvidenceRecord record;
        record.kind = RelationshipEvidenceKind::Settling;
        record.event.entityId = entityId;
        record.event.evidenceId = actions::NewActionId();
        record.originalEntityId = entityId;
        record.observedAt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        snapshot.relationshipEvidence.push_back(std::move(record));
        relationship = SettleRelationship(relationship);
        clock->second = now;
    }
    CompactEvidenceLocked();
}

std::vector<RelationshipEvidenceRecord> RelationshipRegistry::RetainedEvidence() const
{
    std::lock_guard lock(mutex);
    return snapshot.relationshipEvidence;
}

void RelationshipRegistry::CompactEvidenceLocked()
{
    while (snapshot.relationshipEvidence.size() > MaximumEvidence)
    {
        const auto& record = snapshot.relationshipEvidence.front();
        auto& base = snapshot.relationshipEvidenceBase[record.event.entityId];
        base.entityId = record.event.entityId;
        base = ApplyRecord(base, record);
        snapshot.relationshipEvidence.erase(snapshot.relationshipEvidence.begin());
    }
}

void RelationshipRegistry::ReplayEvidenceLocked()
{
    const auto current = snapshot.relationships;
    snapshot.relationships = snapshot.relationshipEvidenceBase;
    for (const auto& record : snapshot.relationshipEvidence)
    {
        auto& relationship = snapshot.relationships[record.event.entityId];
        relationship.entityId = record.event.entityId;
        relationship = ApplyRecord(relationship, record);
    }
    for (const auto& [id, relationship] : current)
    {
        auto& replayed = snapshot.relationships[id];
        if (replayed.entityId.empty())
            replayed.entityId = id;
        replayed.displayName = relationship.displayName;
    }
}

bool RelationshipRegistry::CorrectEvidence(const std::string& evidenceId, const std::string& correctEntityId, std::string& outError)
{
    std::lock_guard lock(mutex);
    const auto record = std::find_if(snapshot.relationshipEvidence.begin(), snapshot.relationshipEvidence.end(),
        [&evidenceId](const auto& entry) { return entry.event.evidenceId == evidenceId; });
    if (!ValidText(correctEntityId, 160) || record == snapshot.relationshipEvidence.end() ||
        record->kind != RelationshipEvidenceKind::Interaction)
    {
        outError = "The referenced social evidence is unavailable for correction.";
        return false;
    }
    if (record->event.entityId == correctEntityId)
        return true;
    RelationshipState fresh;
    fresh.entityId = correctEntityId;
    snapshot.relationshipEvidenceBase.try_emplace(correctEntityId, fresh);
    record->event.entityId = correctEntityId;
    record->corrected = true;
    ReplayEvidenceLocked();
    return true;
}

void RelationshipRegistry::SetDisplayName(const std::string& entityId, const std::string& displayName)
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
    if (auto base = snapshot.relationshipEvidenceBase.find(entityId); base != snapshot.relationshipEvidenceBase.end())
        base->second.displayName = displayName;
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
    const bool anyNamedLocal = std::any_of(snapshot.relationships.begin(), snapshot.relationships.end(),
        [](const auto& entry) { return entry.first.rfind("local:", 0) == 0 && entry.first != LocalUserEntityId(); });

    const auto anonymous = snapshot.relationships.find(LocalUserEntityId());
    if (!anyNamedLocal && anonymous != snapshot.relationships.end())
    {
        RelationshipState adopted = anonymous->second;
        adopted.entityId = target;
        adopted.displayName = name;
        snapshot.relationships.erase(anonymous);
        snapshot.relationships.emplace(target, adopted);
        if (const auto base = snapshot.relationshipEvidenceBase.find(LocalUserEntityId()); base != snapshot.relationshipEvidenceBase.end())
        {
            RelationshipState renamed = base->second;
            renamed.entityId = target;
            renamed.displayName = name;
            snapshot.relationshipEvidenceBase.erase(base);
            snapshot.relationshipEvidenceBase.emplace(target, std::move(renamed));
        }
        for (auto& record : snapshot.relationshipEvidence)
            if (record.event.entityId == LocalUserEntityId())
                record.event.entityId = target;
        if (const auto consent = snapshot.recognitionConsents.find(LocalUserEntityId()); consent != snapshot.recognitionConsents.end())
        {
            snapshot.recognitionConsents[target] = consent->second;
            snapshot.recognitionConsents.erase(consent);
        }
        if (const auto voice = snapshot.speakerTemplates.find(LocalUserEntityId()); voice != snapshot.speakerTemplates.end())
        {
            auto renamed = voice->second;
            renamed.entityId = target;
            snapshot.speakerTemplates.erase(voice);
            snapshot.speakerTemplates[target] = std::move(renamed);
        }
        const auto clock = frictionUpdatedAt.find(LocalUserEntityId());
        if (clock != frictionUpdatedAt.end())
        {
            frictionUpdatedAt[target] = clock->second;
            frictionUpdatedAt.erase(clock);
        }
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

std::string RelationshipRegistry::DefaultLocalSpeaker() const
{
    return LocalUserEntityId();
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
        snapshot.developmentHistory.erase(snapshot.developmentHistory.begin(),
            snapshot.developmentHistory.begin() + static_cast<std::ptrdiff_t>(snapshot.developmentHistory.size() - maximumHistory));
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

bool RelationshipRegistry::SetAudienceAlias(const std::string& entityId, const std::string& audienceId,
    const std::string& recipientEntityId, const std::string& alias, std::string& outError)
{
    if (!ValidText(entityId, 160) || !ValidText(audienceId, 160) || !ValidText(recipientEntityId, 160) || !ValidText(alias, 80))
    {
        outError = "A valid person, audience, recipient and alias are required.";
        return false;
    }
    std::lock_guard lock(mutex);
    auto found = std::find_if(snapshot.audienceAliases.begin(), snapshot.audienceAliases.end(), [&](const auto& entry)
        { return entry.entityId == entityId && entry.audienceId == audienceId && entry.recipientEntityId == recipientEntityId; });
    if (found != snapshot.audienceAliases.end())
    {
        found->alias = alias;
        return true;
    }
    if (snapshot.audienceAliases.size() >= MaximumAliases)
    {
        outError = "The private alias capacity has been reached.";
        return false;
    }
    snapshot.audienceAliases.push_back({entityId, audienceId, recipientEntityId, alias});
    return true;
}

std::optional<std::string> RelationshipRegistry::DisplayNameForAudience(const std::string& entityId, const AudienceContext& audience) const
{
    if (audience.kind == AudienceKind::Unknown || audience.recipientEntityIds.size() > 16)
        return std::nullopt;
    std::lock_guard lock(mutex);
    std::optional<std::string> common;
    for (const auto& recipient : audience.recipientEntityIds)
    {
        const auto alias = std::find_if(snapshot.audienceAliases.begin(), snapshot.audienceAliases.end(), [&](const auto& entry)
            { return entry.entityId == entityId && entry.audienceId == audience.audienceId && entry.recipientEntityId == recipient; });
        if (alias == snapshot.audienceAliases.end() || (common && *common != alias->alias))
            return std::nullopt;
        common = alias->alias;
    }
    if (common)
        return common;
    if (audience.kind == AudienceKind::Private)
    {
        const auto person = snapshot.relationships.find(entityId);
        if (person != snapshot.relationships.end() && !person->second.displayName.empty())
            return person->second.displayName;
    }
    return std::nullopt;
}

bool RelationshipRegistry::GrantRecognitionConsent(const std::string& entityId, std::string& outError)
{
    std::lock_guard lock(mutex);
    if (!ValidText(entityId, 160) || !snapshot.relationships.contains(entityId) ||
        (!snapshot.recognitionConsents.contains(entityId) && snapshot.recognitionConsents.size() >= MaximumConsents))
    {
        outError = "A known person and available consent capacity are required.";
        return false;
    }
    auto& consent = snapshot.recognitionConsents[entityId];
    if (consent.granted)
        return true;
    if (consent.revision == std::numeric_limits<std::uint64_t>::max())
    {
        outError = "The consent revision cannot be advanced.";
        return false;
    }
    ++consent.revision;
    consent.granted = true;
    snapshot.speakerTemplates.erase(entityId);
    return true;
}

bool RelationshipRegistry::RevokeRecognitionConsent(const std::string& entityId, std::string& outError)
{
    std::lock_guard lock(mutex);
    if (!ValidText(entityId, 160) || !snapshot.relationships.contains(entityId) ||
        (!snapshot.recognitionConsents.contains(entityId) && snapshot.recognitionConsents.size() >= MaximumConsents))
    {
        outError = "A known person and available consent capacity are required.";
        return false;
    }
    auto& consent = snapshot.recognitionConsents[entityId];
    if (!consent.granted && consent.revision > 0)
        return true;
    if (consent.revision == std::numeric_limits<std::uint64_t>::max())
    {
        outError = "The consent revision cannot be advanced.";
        return false;
    }
    ++consent.revision;
    consent.granted = false;
    snapshot.speakerTemplates.erase(entityId);
    return true;
}

std::optional<std::uint64_t> RelationshipRegistry::RecognitionConsentRevision(const std::string& entityId) const
{
    std::lock_guard lock(mutex);
    const auto found = snapshot.recognitionConsents.find(entityId);
    if (found == snapshot.recognitionConsents.end() || !found->second.granted)
        return std::nullopt;
    return found->second.revision;
}

std::map<std::string, std::uint64_t> RelationshipRegistry::RecognitionConsentRevisions() const
{
    std::lock_guard lock(mutex);
    std::map<std::string, std::uint64_t> active;
    for (const auto& [entity, consent] : snapshot.recognitionConsents)
        if (consent.granted)
            active.emplace(entity, consent.revision);
    return active;
}

bool RelationshipRegistry::SetSpeakerTemplate(
    const std::string& entityId, const std::uint64_t consentRevision, const speech::SpeakerFeatures& features, std::string& outError)
{
    std::lock_guard lock(mutex);
    const auto consent = snapshot.recognitionConsents.find(entityId);
    if (!ValidFeatures(features) || consent == snapshot.recognitionConsents.end() || !consent->second.granted ||
        consent->second.revision != consentRevision)
    {
        outError = "Current explicit recognition consent and valid features are required.";
        return false;
    }
    snapshot.speakerTemplates[entityId] = {entityId, consentRevision, features};
    return true;
}

bool RelationshipRegistry::SaveConsentedSpeakerSample(
    const std::string& entityId, const speech::SpeakerFeatures& features, const std::function<bool()>& admission, std::string& outError)
{
    std::lock_guard lock(mutex);
    const auto admitted = [&]
    {
        try
        {
            return admission && admission();
        }
        catch (...)
        {
            return false;
        }
    };
    if (!admitted())
    {
        outError = "Recognition enrollment is no longer admitted.";
        return false;
    }
    if (!ValidText(entityId, 160) || !snapshot.relationships.contains(entityId) || !ValidFeatures(features) ||
        (!snapshot.recognitionConsents.contains(entityId) && snapshot.recognitionConsents.size() >= MaximumConsents) ||
        (!snapshot.speakerTemplates.contains(entityId) && snapshot.speakerTemplates.size() >= MaximumConsents))
    {
        outError = "A known person, valid features and available recognition capacity are required.";
        return false;
    }
    IdentitySnapshot candidate = snapshot;
    auto& consent = candidate.recognitionConsents[entityId];
    if (!consent.granted)
    {
        if (consent.revision == std::numeric_limits<std::uint64_t>::max())
        {
            outError = "The consent revision cannot be advanced.";
            return false;
        }
        ++consent.revision;
        consent.granted = true;
    }
    candidate.speakerTemplates[entityId] = {entityId, consent.revision, features};
    if (!admitted())
    {
        outError = "Recognition enrollment is no longer admitted.";
        return false;
    }
    try
    {
        if (!store.Save(candidate, outError))
            return false;
    }
    catch (...)
    {
        outError = "The consented speaker sample could not be saved.";
        return false;
    }
    snapshot = std::move(candidate);
    outError.clear();
    return true;
}

void RelationshipRegistry::DeleteSpeakerTemplate(const std::string& entityId)
{
    std::lock_guard lock(mutex);
    snapshot.speakerTemplates.erase(entityId);
}

std::vector<speech::SpeakerTemplate> RelationshipRegistry::SpeakerTemplates() const
{
    std::lock_guard lock(mutex);
    std::vector<speech::SpeakerTemplate> templates;
    for (const auto& [entity, voice] : snapshot.speakerTemplates)
    {
        const auto consent = snapshot.recognitionConsents.find(entity);
        if (consent != snapshot.recognitionConsents.end() && consent->second.granted && consent->second.revision == voice.consentRevision)
            templates.push_back(voice);
    }
    return templates;
}

} // namespace revia::identity
