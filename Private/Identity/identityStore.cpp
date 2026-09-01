#include "Identity/identityStore.h"

#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

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
}

IdentityStore::IdentityStore(std::filesystem::path path)
    : storePath(std::move(path))
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
        json document;
        file >> document;
        if (document.is_object() && document.contains("schemaVersion") &&
            document["schemaVersion"].is_number_integer())
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
        // First run. The childlike baseline is the correct answer, not an error.
        return true;
    }

    json document;
    try
    {
        file >> document;
    }
    catch (const std::exception& error)
    {
        // Refused rather than replaced. Overwriting a corrupt identity with a fresh one
        // would quietly delete everything she had become, and the user would only find
        // out by noticing she felt different.
        outError = std::string("The identity file could not be parsed: ") + error.what();
        return false;
    }
    if (!document.is_object())
    {
        outError = "The identity file is not an object.";
        return false;
    }

    const int version = document.value("schemaVersion", 0);
    if (version > IdentitySchemaVersion)
    {
        outError = "The identity file was written by a newer build (schema " +
            std::to_string(version) + ").";
        return false;
    }

    if (document.contains("development") && document["development"].is_object())
    {
        const json& development = document["development"];
        outSnapshot.development.base =
            ReadTraits(development.value("base", json::object()), ChildlikeBaseline());
        outSnapshot.development.delta =
            ReadTraits(development.value("delta", json::object()), TraitVector{});
    }

    if (document.contains("mood") && document["mood"].is_object())
    {
        const json& mood = document["mood"];
        outSnapshot.mood.valence = ReadFloat(mood, "valence", 0.0F);
        outSnapshot.mood.energy = ReadFloat(mood, "energy", 0.45F);
        outSnapshot.mood.irritability = ReadFloat(mood, "irritability", 0.0F);
        outSnapshot.mood.sociability = ReadFloat(mood, "sociability", 0.6F);
        outSnapshot.mood.baselineValence = ReadFloat(mood, "baselineValence", 0.0F);
        outSnapshot.mood.baselineEnergy = ReadFloat(mood, "baselineEnergy", 0.45F);
        outSnapshot.mood.baselineSociability =
            ReadFloat(mood, "baselineSociability", 0.6F);
    }

    if (document.contains("relationships") && document["relationships"].is_array())
    {
        for (const json& entry : document["relationships"])
        {
            if (!entry.is_object()) continue;
            RelationshipState relationship = ReadRelationship(entry);
            if (relationship.entityId.empty()) continue;
            outSnapshot.relationships.emplace(relationship.entityId, relationship);
        }
    }

    // Absent in schema 1. A file without them is a personality that has not formed any
    // opinions yet, which is a valid state rather than an error.
    if (document.contains("preferences") && document["preferences"].is_array())
    {
        for (const json& entry : document["preferences"])
        {
            if (!entry.is_object()) continue;
            Preference preference;
            preference.subject =
                PreferenceSet::NormaliseSubject(entry.value("subject", std::string{}));
            if (preference.subject.empty()) continue;
            preference.strength = ReadFloat(entry, "strength", 0.0F);
            preference.confidence = ReadFloat(entry, "confidence", 0.0F);
            preference.evidenceCount = entry.value("evidenceCount", std::size_t{0});
            preference.lastReinforced = entry.value("lastReinforced", std::string{});
            preference.source =
                PreferenceSourceFromString(entry.value("source", std::string{}));
            outSnapshot.preferences.push_back(std::move(preference));
        }
    }

    if (document.contains("developmentHistory") && document["developmentHistory"].is_array())
    {
        for (const json& entry : document["developmentHistory"])
        {
            if (!entry.is_object()) continue;
            DevelopmentChange change;
            const Trait trait = TraitFromString(entry.value("trait", std::string{}));
            if (trait == Trait::Count) continue;
            change.trait = trait;
            change.delta = ReadFloat(entry, "delta", 0.0F);
            change.reason = entry.value("reason", std::string{});
            change.evidenceCount = entry.value("evidenceCount", std::size_t{0});
            change.recordedAt = entry.value("recordedAt", std::string{});
            outSnapshot.developmentHistory.push_back(std::move(change));
        }
    }
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
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            outError = "The identity file could not be opened for writing.";
            return false;
        }
        output << document.dump(2) << "\n";
        if (!output.good())
        {
            outError = "The identity file could not be written.";
            return false;
        }
    }
    std::filesystem::rename(temporary, storePath, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        outError = "The identity file could not be replaced.";
        return false;
    }
    return true;
}

} // namespace revia::identity
