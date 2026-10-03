#include "Identity/identityStore.h"

#include <exception>
#include <algorithm>
#include <cmath>
#include <set>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace revia::identity
{

namespace
{
using json = nlohmann::json;

float ReadFloat(const json& source, const char* key, const float fallback)
{
    if (!source.contains(key) || !source[key].is_number())
    {
        return fallback;
    }
    return source[key].get<float>();
}

json WriteTraits(const TraitVector& traits)
{
    json output = json::object();
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        output[TraitNames()[index]] = traits.values[index];
    }
    return output;
}

// Unknown names are skipped rather than rejected, so a file written by a build that
// knew about one more trait still loads here with that trait simply absent.
TraitVector ReadTraits(const json& source, const TraitVector& fallback)
{
    TraitVector traits = fallback;
    if (!source.is_object())
    {
        return traits;
    }
    for (const auto& [name, value] : source.items())
    {
        const Trait trait = TraitFromString(name);
        if (trait != Trait::Count && value.is_number())
        {
            traits[trait] = value.get<float>();
        }
    }
    return traits;
}

json WriteRelationship(const RelationshipState& relationship)
{
    json output;
    output["entityId"] = relationship.entityId;
    output["displayName"] = relationship.displayName;
    output["familiarity"] = relationship.familiarity;
    output["affinity"] = relationship.affinity;
    output["trust"] = relationship.trust;
    output["respect"] = relationship.respect;
    output["comfort"] = relationship.comfort;
    output["attachment"] = relationship.attachment;
    output["irritation"] = relationship.irritation;
    output["resentment"] = relationship.resentment;
    output["admiration"] = relationship.admiration;
    output["playfulness"] = relationship.playfulness;
    output["interactionCount"] = relationship.interactionCount;
    output["firstSeenAt"] = relationship.firstSeenAt;
    output["lastSeenAt"] = relationship.lastSeenAt;
    return output;
}

RelationshipState ReadRelationship(const json& source)
{
    RelationshipState relationship;
    relationship.entityId = source.value("entityId", std::string{});
    relationship.displayName = source.value("displayName", std::string{});
    relationship.familiarity = ReadFloat(source, "familiarity", 0.0F);
    relationship.affinity = ReadFloat(source, "affinity", 0.0F);
    relationship.trust = ReadFloat(source, "trust", 0.25F);
    relationship.respect = ReadFloat(source, "respect", 0.25F);
    relationship.comfort = ReadFloat(source, "comfort", 0.2F);
    relationship.attachment = ReadFloat(source, "attachment", 0.0F);
    relationship.irritation = ReadFloat(source, "irritation", 0.0F);
    relationship.resentment = ReadFloat(source, "resentment", 0.0F);
    relationship.admiration = ReadFloat(source, "admiration", 0.0F);
    relationship.playfulness = ReadFloat(source, "playfulness", 0.2F);
    relationship.interactionCount = source.value("interactionCount", std::uint64_t{0});
    relationship.firstSeenAt = source.value("firstSeenAt", std::string{});
    relationship.lastSeenAt = source.value("lastSeenAt", std::string{});
    return relationship;
}

bool ValidText(const std::string& value, const std::size_t limit)
{
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(), [](const unsigned char byte) { return byte < 32 || byte == 127; });
}

void Require(const bool condition)
{
    if (!condition)
        throw std::runtime_error("Invalid social identity field.");
}

json WriteEvent(const RelationshipEvent& event)
{
    return {{"entityId", event.entityId}, {"evidenceId", event.evidenceId}, {"positive", event.positiveInteraction},
        {"negative", event.negativeInteraction}, {"trust", event.trustEvidence}, {"disrespect", event.disrespectEvidence},
        {"cooperation", event.cooperation}, {"conflict", event.conflict}, {"importance", event.importance},
        {"confidence", event.confidence}, {"description", event.description}};
}

RelationshipEvent ReadEvent(const json& source)
{
    RelationshipEvent event;
    event.entityId = source.at("entityId").get<std::string>();
    event.evidenceId = source.at("evidenceId").get<std::string>();
    event.positiveInteraction = source.at("positive").get<float>();
    event.negativeInteraction = source.at("negative").get<float>();
    event.trustEvidence = source.at("trust").get<float>();
    event.disrespectEvidence = source.at("disrespect").get<float>();
    event.cooperation = source.at("cooperation").get<float>();
    event.conflict = source.at("conflict").get<float>();
    event.importance = source.at("importance").get<float>();
    event.confidence = source.at("confidence").get<float>();
    event.description = source.at("description").get<std::string>();
    Require(ValidText(event.entityId, 160) && ValidText(event.evidenceId, 160) && event.description.size() <= 512);
    for (const float value : {event.positiveInteraction, event.negativeInteraction, event.trustEvidence, event.disrespectEvidence,
             event.cooperation, event.conflict, event.importance, event.confidence})
        Require(std::isfinite(value) && std::abs(value) <= 1.0F);
    return event;
}

void ReadSocial(const json& document, IdentitySnapshot& decoded)
{
    const json bases = document.value("relationshipEvidenceBase", json::array());
    const json records = document.value("relationshipEvidence", json::array());
    const json aliases = document.value("audienceAliases", json::array());
    const json consents = document.value("recognitionConsents", json::array());
    const json templates = document.value("speakerTemplates", json::array());
    Require(bases.is_array() && records.is_array() && aliases.is_array() && consents.is_array() && templates.is_array());
    Require(bases.size() <= 4096 && records.size() <= 256 && aliases.size() <= 512 && consents.size() <= 128 && templates.size() <= 128);
    for (const auto& entry : bases)
    {
        const auto state = ReadRelationship(entry);
        Require(ValidText(state.entityId, 160) && decoded.relationshipEvidenceBase.emplace(state.entityId, state).second);
    }
    std::set<std::string> receipts;
    for (const auto& entry : records)
    {
        RelationshipEvidenceRecord record;
        record.event = ReadEvent(entry.at("event"));
        const auto kind = entry.at("kind").get<std::string>();
        Require(kind == "interaction" || kind == "settling");
        record.kind = kind == "settling" ? RelationshipEvidenceKind::Settling : RelationshipEvidenceKind::Interaction;
        record.originalEntityId = entry.at("originalEntityId").get<std::string>();
        record.observedAt = entry.at("observedAt").get<std::int64_t>();
        record.corrected = entry.at("corrected").get<bool>();
        Require(ValidText(record.originalEntityId, 160) && receipts.insert(record.event.evidenceId).second &&
                decoded.relationships.contains(record.event.entityId) && decoded.relationshipEvidenceBase.contains(record.event.entityId));
        decoded.relationshipEvidence.push_back(std::move(record));
    }
    std::set<std::tuple<std::string, std::string, std::string>> aliasKeys;
    for (const auto& entry : aliases)
    {
        AudienceAlias alias{entry.at("entityId").get<std::string>(), entry.at("audienceId").get<std::string>(),
            entry.at("recipientEntityId").get<std::string>(), entry.at("alias").get<std::string>()};
        Require(ValidText(alias.entityId, 160) && ValidText(alias.audienceId, 160) && ValidText(alias.recipientEntityId, 160) &&
                ValidText(alias.alias, 80) && aliasKeys.emplace(alias.entityId, alias.audienceId, alias.recipientEntityId).second);
        decoded.audienceAliases.push_back(std::move(alias));
    }
    for (const auto& entry : consents)
    {
        const auto entity = entry.at("entityId").get<std::string>();
        RecognitionConsent consent{entry.at("granted").get<bool>(), entry.at("revision").get<std::uint64_t>()};
        Require(ValidText(entity, 160) && consent.revision > 0 && decoded.relationships.contains(entity) &&
                decoded.recognitionConsents.emplace(entity, consent).second);
    }
    for (const auto& entry : templates)
    {
        speech::SpeakerTemplate value;
        value.entityId = entry.at("entityId").get<std::string>();
        value.consentRevision = entry.at("consentRevision").get<std::uint64_t>();
        value.features.version = entry.at("version").get<std::uint32_t>();
        const auto features = entry.at("values").get<std::vector<float>>();
        Require(features.size() == speech::SpeakerFeatureCount && value.features.version == speech::SpeakerFeatureVersion);
        double norm = 0;
        for (std::size_t index = 0; index < features.size(); ++index)
        {
            Require(std::isfinite(features[index]) && std::abs(features[index]) <= 1);
            value.features.values[index] = features[index];
            norm += static_cast<double>(features[index]) * features[index];
        }
        const auto consent = decoded.recognitionConsents.find(value.entityId);
        Require(norm > 0 && consent != decoded.recognitionConsents.end() && consent->second.granted &&
                consent->second.revision == value.consentRevision && decoded.speakerTemplates.emplace(value.entityId, value).second);
    }
}

void WriteSocial(json& document, const IdentitySnapshot& snapshot)
{
    for (const auto name :
        {"relationshipEvidenceBase", "relationshipEvidence", "audienceAliases", "recognitionConsents", "speakerTemplates"})
        document[name] = json::array();
    for (const auto& [id, state] : snapshot.relationshipEvidenceBase)
        document["relationshipEvidenceBase"].push_back(WriteRelationship(state));
    for (const auto& record : snapshot.relationshipEvidence)
        document["relationshipEvidence"].push_back(
            {{"event", WriteEvent(record.event)}, {"kind", record.kind == RelationshipEvidenceKind::Settling ? "settling" : "interaction"},
                {"originalEntityId", record.originalEntityId}, {"observedAt", record.observedAt}, {"corrected", record.corrected}});
    for (const auto& alias : snapshot.audienceAliases)
        document["audienceAliases"].push_back({{"entityId", alias.entityId}, {"audienceId", alias.audienceId},
            {"recipientEntityId", alias.recipientEntityId}, {"alias", alias.alias}});
    for (const auto& [id, consent] : snapshot.recognitionConsents)
        document["recognitionConsents"].push_back({{"entityId", id}, {"granted", consent.granted}, {"revision", consent.revision}});
    for (const auto& [id, value] : snapshot.speakerTemplates)
        document["speakerTemplates"].push_back({{"entityId", id}, {"consentRevision", value.consentRevision},
            {"version", value.features.version}, {"values", value.features.values}});
}
}

IdentityStore::IdentityStore(std::filesystem::path path) : storePath(std::move(path))
{
}

std::optional<int> IdentityStore::StoredVersion() const
{
    std::lock_guard lock(mutex);
    std::ifstream file(storePath);
    if (!file.is_open())
    {
        return std::nullopt;
    }
    try
    {
        const json document = json::parse(file);
        if (!file.eof())
            return std::nullopt;
        if (document.is_object() && document.contains("schemaVersion") && document["schemaVersion"].is_number_integer())
        {
            return document["schemaVersion"].get<int>();
        }
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    return std::nullopt;
}

bool IdentityStore::Load(IdentitySnapshot& outSnapshot, std::string& outError) const
{
    std::lock_guard lock(mutex);
    outSnapshot = IdentitySnapshot{};

    std::ifstream file(storePath);
    if (!file.is_open())
    {
        std::error_code error;
        if (!std::filesystem::exists(storePath, error) && !error)
            return true;
        outError = "The existing identity file could not be opened for reading.";
        return false;
    }

    json document;
    try
    {
        document = json::parse(file);
        if (!file.eof())
            throw std::runtime_error("Trailing identity data.");
    }
    catch (const std::exception&)
    {
        // Refused rather than replaced. Overwriting a corrupt identity with a fresh one
        // would quietly delete everything she had become, and the user would only find
        // out by noticing she felt different.
        outError = "The identity file could not be parsed completely.";
        return false;
    }
    // Decoded into a temporary, and only adopted once it is whole.
    //
    // nlohmann throws when a field holds the wrong type, so valid JSON with a
    // mistyped field -- {"schemaVersion":"2"} rather than {"schemaVersion":2} --
    // threw out of Load instead of returning a controlled failure, and whatever had
    // already been decoded was left in the caller's snapshot. A file Revia cannot
    // read is a reason to say so and keep the file, never a reason to end the turn
    // or to hand back half an identity.
    IdentitySnapshot decoded;
    try
    {
        if (!document.is_object())
        {
            outError = "The identity file is not an object.";
            return false;
        }

        const int version = document.value("schemaVersion", 0);
        if (version > IdentitySchemaVersion)
        {
            outError = "The identity file was written by a newer build (schema " + std::to_string(version) + ").";
            return false;
        }

        if (document.contains("development") && document["development"].is_object())
        {
            const json& development = document["development"];
            decoded.development.base = ReadTraits(development.value("base", json::object()), ChildlikeBaseline());
            decoded.development.delta = ReadTraits(development.value("delta", json::object()), TraitVector{});
        }

        if (document.contains("mood") && document["mood"].is_object())
        {
            const json& mood = document["mood"];
            decoded.mood.valence = ReadFloat(mood, "valence", 0.0F);
            decoded.mood.energy = ReadFloat(mood, "energy", 0.45F);
            decoded.mood.irritability = ReadFloat(mood, "irritability", 0.0F);
            decoded.mood.sociability = ReadFloat(mood, "sociability", 0.6F);
            decoded.mood.baselineValence = ReadFloat(mood, "baselineValence", 0.0F);
            decoded.mood.baselineEnergy = ReadFloat(mood, "baselineEnergy", 0.45F);
            decoded.mood.baselineSociability = ReadFloat(mood, "baselineSociability", 0.6F);
        }

        if (document.contains("relationships") && document["relationships"].is_array())
        {
            for (const json& entry : document["relationships"])
            {
                if (!entry.is_object())
                    continue;
                RelationshipState relationship = ReadRelationship(entry);
                if (relationship.entityId.empty())
                    continue;
                decoded.relationships.emplace(relationship.entityId, relationship);
            }
        }

        // Absent in schema 1. A file without them is a personality that has not formed any
        // opinions yet, which is a valid state rather than an error.
        if (document.contains("preferences") && document["preferences"].is_array())
        {
            for (const json& entry : document["preferences"])
            {
                if (!entry.is_object())
                    continue;
                Preference preference;
                preference.subject = PreferenceSet::NormaliseSubject(entry.value("subject", std::string{}));
                if (preference.subject.empty())
                    continue;
                preference.strength = ReadFloat(entry, "strength", 0.0F);
                preference.confidence = ReadFloat(entry, "confidence", 0.0F);
                preference.evidenceCount = entry.value("evidenceCount", std::size_t{0});
                preference.lastReinforced = entry.value("lastReinforced", std::string{});
                preference.source = PreferenceSourceFromString(entry.value("source", std::string{}));
                decoded.preferences.push_back(std::move(preference));
            }
        }

        if (document.contains("developmentHistory") && document["developmentHistory"].is_array())
        {
            for (const json& entry : document["developmentHistory"])
            {
                if (!entry.is_object())
                    continue;
                DevelopmentChange change;
                const Trait trait = TraitFromString(entry.value("trait", std::string{}));
                if (trait == Trait::Count)
                    continue;
                change.trait = trait;
                change.delta = ReadFloat(entry, "delta", 0.0F);
                change.reason = entry.value("reason", std::string{});
                change.evidenceCount = entry.value("evidenceCount", std::size_t{0});
                change.recordedAt = entry.value("recordedAt", std::string{});
                decoded.developmentHistory.push_back(std::move(change));
            }
        }
        ReadSocial(document, decoded);
    }
    catch (const std::exception&)
    {
        outError = "The identity file contains an invalid or unsupported field.";
        return false;
    }

    outSnapshot = std::move(decoded);
    return true;
}

bool IdentityStore::Save(const IdentitySnapshot& snapshot, std::string& outError) const
{
    std::lock_guard lock(mutex);

    json document;
    document["schemaVersion"] = IdentitySchemaVersion;
    document["development"]["base"] = WriteTraits(snapshot.development.base);
    document["development"]["delta"] = WriteTraits(snapshot.development.delta);

    json preferences = json::array();
    for (const Preference& preference : snapshot.preferences)
    {
        json entry;
        entry["subject"] = preference.subject;
        entry["strength"] = preference.strength;
        entry["confidence"] = preference.confidence;
        entry["evidenceCount"] = preference.evidenceCount;
        entry["lastReinforced"] = preference.lastReinforced;
        entry["source"] = ToString(preference.source);
        preferences.push_back(std::move(entry));
    }
    document["preferences"] = std::move(preferences);

    json mood;
    mood["valence"] = snapshot.mood.valence;
    mood["energy"] = snapshot.mood.energy;
    mood["irritability"] = snapshot.mood.irritability;
    mood["sociability"] = snapshot.mood.sociability;
    mood["baselineValence"] = snapshot.mood.baselineValence;
    mood["baselineEnergy"] = snapshot.mood.baselineEnergy;
    mood["baselineSociability"] = snapshot.mood.baselineSociability;
    document["mood"] = std::move(mood);

    json relationships = json::array();
    for (const auto& [entityId, relationship] : snapshot.relationships)
    {
        relationships.push_back(WriteRelationship(relationship));
    }
    document["relationships"] = std::move(relationships);

    json history = json::array();
    for (const DevelopmentChange& change : snapshot.developmentHistory)
    {
        json entry;
        entry["trait"] = ToString(change.trait);
        entry["delta"] = change.delta;
        entry["reason"] = change.reason;
        entry["evidenceCount"] = change.evidenceCount;
        entry["recordedAt"] = change.recordedAt;
        history.push_back(std::move(entry));
    }
    document["developmentHistory"] = std::move(history);
    WriteSocial(document, snapshot);

    std::error_code error;
    const std::filesystem::path parent = storePath.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            outError = "The identity folder could not be created: " + error.message();
            return false;
        }
    }

    // Written beside the target and moved into place. An interrupted save must not be
    // able to leave a truncated identity that the next start reads as a smaller person.
    const std::filesystem::path temporary = storePath.string() + ".tmp";
    // Replaced rather than refused. nlohmann throws on text that is not UTF-8, and a
    // single such byte in a display name -- console input on Windows arrives in the OEM
    // code page -- would otherwise throw out of every later save, so the identity would
    // silently stop being written. One unreadable character is the lesser loss.
    const std::string bytes = document.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
#ifdef _WIN32
    std::FILE* output = _wfopen(temporary.c_str(), L"wb");
#else
    std::FILE* output = std::fopen(temporary.c_str(), "wb");
#endif
    if (!output)
    {
        outError = "The identity file could not be opened for writing.";
        return false;
    }
    bool written = std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size() && std::fflush(output) == 0;
#ifdef _WIN32
    written = written && _commit(_fileno(output)) == 0;
#else
    written = written && fsync(fileno(output)) == 0;
#endif
    const bool closed = std::fclose(output) == 0;
    if (!written || !closed)
    {
        std::filesystem::remove(temporary, error);
        outError = "The identity file could not be flushed and closed; the previous identity was preserved.";
        return false;
    }
#ifdef _WIN32
    const bool replaced = MoveFileExW(temporary.c_str(), storePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, storePath, error);
    const bool replaced = !error;
#endif
    if (!replaced)
    {
        std::filesystem::remove(temporary, error);
        outError = "The identity file could not be replaced.";
        return false;
    }
    outError.clear();
    return true;
}

} // namespace revia::identity
