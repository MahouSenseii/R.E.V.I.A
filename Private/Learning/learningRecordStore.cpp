#include "Learning/learningRecordStore.h"

#include "Audit/contentDigest.h"
#include "Memory/sensitiveContent.h"
#include "../Skills/packageStorage.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

namespace revia::learning
{
namespace
{
using nlohmann::json;

json CandidateJson(const LearningCandidate& value)
{
    const auto& evidence = value.evidence;
    const auto& origin = value.origin;
    return {{"lesson", {{"id", value.lesson.id}, {"kind", static_cast<int>(value.lesson.kind)}, {"statement", value.lesson.statement},
                           {"evidence", value.lesson.evidence}, {"sampleSize", value.lesson.sampleSize}}},
        {"evidence", {{"sources", evidence.sources}, {"conditions", evidence.conditions}, {"supporting", evidence.supporting},
                         {"contradicting", evidence.contradicting}, {"modelVersion", evidence.modelVersion},
                         {"toolVersion", evidence.toolVersion}, {"checkedAt", evidence.checkedAt}}},
        {"origin", {{"companionId", origin.companionId}, {"sessionId", origin.sessionId}, {"generation", origin.generation},
                       {"taskId", origin.taskId}, {"attemptId", origin.attemptId}, {"policyVersion", origin.policyVersion}}},
        {"correctsRecordId", value.correctsRecordId}};
}

void ValidateCandidate(const LearningCandidate& value)
{
    const auto bounded = [](const std::vector<std::string>& values)
    {
        return values.size() <= 32 &&
               std::all_of(values.begin(), values.end(), [](const std::string& item) { return !item.empty() && item.size() <= 1024; });
    };
    if (value.lesson.id.empty() || value.lesson.id.size() > 128 || value.lesson.statement.empty() || value.lesson.statement.size() > 4096 ||
        value.lesson.evidence.empty() || value.lesson.evidence.size() > 4096 || value.lesson.sampleSize == 0 ||
        value.lesson.sampleSize > 1000000 || value.origin.companionId.empty() || value.origin.companionId.size() > 128 ||
        value.origin.sessionId.empty() || value.origin.sessionId.size() > 128 || value.origin.taskId.size() > 128 ||
        value.origin.attemptId.size() > 128 || value.correctsRecordId.size() > 80 || value.evidence.modelVersion.size() > 256 ||
        value.evidence.toolVersion.size() > 256 || value.evidence.checkedAt.empty() || value.evidence.checkedAt.size() > 64 ||
        value.evidence.sources.empty() || value.evidence.conditions.empty() || value.evidence.supporting.empty() ||
        !bounded(value.evidence.sources) || !bounded(value.evidence.conditions) || !bounded(value.evidence.supporting) ||
        !bounded(value.evidence.contradicting) ||
        (value.lesson.kind != LessonKind::Planning && value.lesson.kind != LessonKind::Initiative) ||
        memory::ContainsSensitiveContent(CandidateJson(value).dump()))
        throw std::runtime_error("Learning candidate lacks bounded safe evidence or captured identity.");
}

LearningCandidate DecodeCandidate(const json& data)
{
    LearningCandidate value;
    const auto& lesson = data.at("lesson");
    value.lesson = {lesson.at("id").get<std::string>(), static_cast<LessonKind>(lesson.at("kind").get<int>()),
        lesson.at("statement").get<std::string>(), lesson.at("evidence").get<std::string>(), lesson.at("sampleSize").get<std::size_t>()};
    const auto& evidence = data.at("evidence");
    value.evidence = {evidence.at("sources").get<std::vector<std::string>>(), evidence.at("conditions").get<std::vector<std::string>>(),
        evidence.at("supporting").get<std::vector<std::string>>(), evidence.at("contradicting").get<std::vector<std::string>>(),
        evidence.at("modelVersion").get<std::string>(), evidence.at("toolVersion").get<std::string>(),
        evidence.at("checkedAt").get<std::string>()};
    const auto& origin = data.at("origin");
    value.origin = {origin.at("companionId").get<std::string>(), origin.at("sessionId").get<std::string>(),
        origin.at("generation").get<std::uint64_t>(), origin.at("taskId").get<std::string>(), origin.at("attemptId").get<std::string>(),
        origin.at("policyVersion").get<std::uint64_t>()};
    value.correctsRecordId = data.at("correctsRecordId").get<std::string>();
    ValidateCandidate(value);
    return value;
}

json RecordJson(const LearningRecord& value)
{
    json reviews = json::array();
    for (const auto& review : value.reviews)
        reviews.push_back({{"decision", static_cast<int>(review.decision)}, {"feedback", review.feedback}});
    return {{"id", value.id}, {"digest", value.digest}, {"candidate", CandidateJson(value.candidate)},
        {"checks", {{"candidateDigest", value.checks.candidateDigest}, {"verified", value.checks.verified},
                       {"privacySafe", value.checks.privacySafe}, {"deduplicated", value.checks.deduplicated}}},
        {"reviews", reviews}, {"decision", static_cast<int>(value.decision)}, {"disposition", static_cast<int>(value.disposition)},
        {"memoryId", value.memoryId}};
}

bool Eligible(const LearningRecord& value)
{
    return value.checks.candidateDigest == value.digest && value.checks.verified && value.checks.privacySafe && value.checks.deduplicated;
}
}

LearningRecordStore::LearningRecordStore(std::filesystem::path value, AdmissionGuard guard)
    : directory(std::move(value)), admission(std::move(guard))
{
}

bool LearningRecordStore::Admitted() const
{
    return !admission || admission();
}

bool LearningRecordStore::Save(const std::vector<LearningRecord>& values, std::string& error) const
{
    try
    {
        if (!Admitted())
            throw std::runtime_error("Learning admission belongs to an ended session.");
        json data = {{"schemaVersion", 1}, {"records", json::array()}};
        for (const auto& record : values)
            data["records"].push_back(RecordJson(record));
        const std::string bytes = data.dump(2);
        if (bytes.size() > 2097152)
            throw std::runtime_error("Learning history capacity is exhausted.");
        skills::storage::AtomicWrite(directory / "records.json", bytes);
        error.clear();
        return true;
    }
    catch (...)
    {
        error = "Private learning history could not be saved safely.";
        return false;
    }
}

bool LearningRecordStore::Initialize(std::string& error)
{
    const std::lock_guard lock(mutex);
    try
    {
        skills::storage::CheckPath(directory);
        std::vector<LearningRecord> loaded;
        if (std::filesystem::exists(directory / "records.json"))
        {
            const auto data = skills::storage::ReadJson(directory / "records.json", 2097152);
            if (data.at("schemaVersion") != 1 || !data.at("records").is_array() || data.at("records").size() > 256)
                throw std::runtime_error("Invalid learning history schema.");
            for (const auto& item : data.at("records"))
            {
                LearningRecord record;
                record.candidate = DecodeCandidate(item.at("candidate"));
                record.digest = audit::ContentDigest(CandidateJson(record.candidate).dump());
                record.id = "lesson-" + record.digest;
                if (item.at("digest") != record.digest || item.at("id") != record.id ||
                    std::any_of(loaded.begin(), loaded.end(), [&](const auto& prior) { return prior.id == record.id; }))
                    throw std::runtime_error("Learning history identity mismatch.");
                const auto& checks = item.at("checks");
                record.checks = {checks.at("candidateDigest").get<std::string>(), checks.at("verified").get<bool>(),
                    checks.at("privacySafe").get<bool>(), checks.at("deduplicated").get<bool>()};
                const int decision = item.at("decision").get<int>(), disposition = item.at("disposition").get<int>();
                if (decision < 0 || decision > 4 || disposition < 0 || disposition > 3 || item.at("reviews").size() > 32)
                    throw std::runtime_error("Learning history decision is invalid.");
                record.decision = static_cast<LearningDecision>(decision);
                record.disposition = static_cast<LearningDisposition>(disposition);
                record.memoryId = item.at("memoryId").get<std::string>();
                for (const auto& review : item.at("reviews"))
                {
                    const int verdict = review.at("decision").get<int>();
                    const auto feedback = review.at("feedback").get<std::string>();
                    if (verdict < 1 || verdict > 4 || feedback.size() > 4096)
                        throw std::runtime_error("Invalid learning review.");
                    record.reviews.push_back({static_cast<LearningDecision>(verdict), feedback});
                }
                if ((record.reviews.empty() != (record.decision == LearningDecision::Pending)) ||
                    (!record.reviews.empty() && record.reviews.back().decision != record.decision) ||
                    (record.decision == LearningDecision::Accept && !Eligible(record)) ||
                    (record.disposition != LearningDisposition::PrivateCandidate && record.decision != LearningDecision::Accept) ||
                    (record.disposition == LearningDisposition::TrustedMemory && record.memoryId.empty()) || record.memoryId.size() > 128)
                    throw std::runtime_error("Learning history admission is inconsistent.");
                loaded.push_back(std::move(record));
            }
        }
        records = std::move(loaded);
        initialized = true;
        error.clear();
        return true;
    }
    catch (...)
    {
        error = "Private learning history is invalid or unreadable.";
        return false;
    }
}

bool LearningRecordStore::Propose(const LearningCandidate& candidate, std::string& id, std::string& error)
{
    const std::lock_guard lock(mutex);
    try
    {
        if (!initialized || !Admitted())
            throw std::runtime_error("Learning owner unavailable.");
        ValidateCandidate(candidate);
        LearningRecord record;
        record.candidate = candidate;
        record.digest = audit::ContentDigest(CandidateJson(candidate).dump());
        record.id = "lesson-" + record.digest;
        for (const auto& prior : records)
            if (prior.id == record.id)
            {
                id = prior.id;
                error.clear();
                return true;
            }
        if (records.size() >= 256 ||
            (!candidate.correctsRecordId.empty() &&
                std::none_of(records.begin(), records.end(), [&](const auto& prior) { return prior.id == candidate.correctsRecordId; })))
            throw std::runtime_error("Learning capacity or correction reference invalid.");
        auto updated = records;
        updated.push_back(record);
        if (!Save(updated, error))
            return false;
        records.swap(updated);
        id = record.id;
        return true;
    }
    catch (...)
    {
        error = "Learning candidate could not be recorded safely.";
        return false;
    }
}

bool LearningRecordStore::Check(const std::string& id, const LearningChecks& checks, std::string& error)
{
    const std::lock_guard lock(mutex);
    auto updated = records;
    for (auto& record : updated)
    {
        if (record.id != id)
            continue;
        if (checks.candidateDigest != record.digest || record.decision == LearningDecision::Accept ||
            record.decision == LearningDecision::Reject)
            break;
        record.checks = checks;
        if (!Save(updated, error))
            return false;
        records.swap(updated);
        return true;
    }
    error = "Learning checks do not match an open candidate.";
    return false;
}

bool LearningRecordStore::Decide(const std::string& id, const LearningDecision decision, const std::string& feedback, std::string& error)
{
    const std::lock_guard lock(mutex);
    auto updated = records;
    for (auto& record : updated)
    {
        if (record.id != id)
            continue;
        if (decision < LearningDecision::Accept || decision > LearningDecision::NeedEvidence || feedback.empty() ||
            feedback.size() > 4096 || memory::ContainsSensitiveContent(feedback) || record.reviews.size() >= 32 ||
            record.decision == LearningDecision::Accept || record.decision == LearningDecision::Reject ||
            (decision == LearningDecision::Accept && !Eligible(record)))
            break;
        record.decision = decision;
        record.reviews.push_back({decision, feedback});
        record.disposition =
            decision == LearningDecision::Accept ? LearningDisposition::AcceptedAwaitingMemory : LearningDisposition::PrivateCandidate;
        if (!Save(updated, error))
            return false;
        records.swap(updated);
        return true;
    }
    error = "Learning decision is ineligible or does not match an open candidate.";
    return false;
}

std::optional<LearningRecord> LearningRecordStore::Find(const std::string& id) const
{
    const std::lock_guard lock(mutex);
    for (const auto& record : records)
        if (record.id == id)
            return record;
    return std::nullopt;
}

std::vector<LearningRecord> LearningRecordStore::History() const
{
    const std::lock_guard lock(mutex);
    return records;
}

std::optional<LearningRecord> LearningRecordStore::PendingAdmission(const std::string& id) const
{
    const std::lock_guard lock(mutex);
    if (!Admitted())
        return std::nullopt;
    for (const auto& record : records)
        if (record.id == id && record.decision == LearningDecision::Accept && Eligible(record) &&
            record.disposition != LearningDisposition::TrustedMemory)
            return record;
    return std::nullopt;
}

bool LearningRecordStore::RecordAdmission(const std::string& id, const bool saved, const std::string& memoryId, std::string& error)
{
    const std::lock_guard lock(mutex);
    auto updated = records;
    for (auto& record : updated)
    {
        if (record.id != id)
            continue;
        if (record.decision != LearningDecision::Accept || !Eligible(record) || record.disposition == LearningDisposition::TrustedMemory ||
            (saved && (memoryId.empty() || memoryId.size() > 128)))
            break;
        record.disposition = saved ? LearningDisposition::TrustedMemory : LearningDisposition::MemorySaveFailed;
        record.memoryId = saved ? memoryId : std::string{};
        if (!Save(updated, error))
            return false;
        records.swap(updated);
        return true;
    }
    error = "Learning receipt does not match eligible uncommitted content.";
    return false;
}

} // namespace revia::learning
