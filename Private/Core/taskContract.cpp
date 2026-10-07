#include "Core/evidenceBundle.h"
#include "Actions/actionTypes.h"
#include "Runtime/turnContext.h"
#include "Core/utf8.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace revia::core
{
namespace
{
using nlohmann::json;
constexpr std::size_t MaximumStringBytes = 8192;
constexpr std::size_t MaximumItems = 64;

json EncodeTask(const TaskContract& contract);
json EncodeReference(const EvidenceRef& reference);

struct ValidationFailure
{
    ContractValidation validation;
};

ContractValidation Good()
{
    return {true, {}, {}, {}};
}

ContractValidation Bad(std::string code, std::string field, std::string message)
{
    return {false, std::move(code), std::move(field), std::move(message)};
}

void Require(bool condition, const char* code, const std::string& field, const char* message)
{
    if (!condition)
    {
        throw ValidationFailure{Bad(code, field, message)};
    }
}

void Version(const SchemaVersion& version)
{
    Require(version.major == 1, "unsupported_major", "version.major", "Only schema major 1 is supported.");
    Require(version.minor == 0, "unsupported_minor", "version.minor", "Only schema minor 0 is supported.");
}

void Text(const std::string& text, const std::string& field, bool required = true)
{
    Require(text.size() <= MaximumStringBytes, "limit_exceeded", field, "Text exceeds the byte limit.");
    Require(!required || text.find_first_not_of(" \t\r\n") != std::string::npos, "missing_value", field, "A nonempty value is required.");
    Require(text.find('\0') == std::string::npos, "invalid_value", field, "Embedded NUL is not permitted.");
    Require(utf8::IsValid(text), "invalid_utf8", field, "Text must be valid UTF8.");
}

void Strings(const std::vector<std::string>& values, const std::string& field, bool required = false)
{
    Require(values.size() <= MaximumItems, "limit_exceeded", field, "Array exceeds the item limit.");
    Require(!required || !values.empty(), "missing_value", field, "At least one value is required.");
    for (const auto& value : values)
    {
        Text(value, field);
    }
}

void Stamp(const runtime::RuntimeStamp& stamp, const std::string& field)
{
    Text(stamp.companionId, field + ".companionId");
    Text(stamp.sessionId, field + ".sessionId");
    Text(stamp.taskId, field + ".taskId");
    Text(stamp.attemptId, field + ".attemptId");
}

void Scope(const memory::MemoryScope& scope, const runtime::RuntimeStamp& stamp)
{
    Text(scope.companionId, "scope.companionId");
    Require(scope.companionId == stamp.companionId, "scope_mismatch", "scope.companionId", "Scope belongs to a different companion.");
    Text(scope.participantId, "scope.participantId", false);
    Text(scope.audience.audienceId, "scope.audience.audienceId");
    Require(scope.audience.kind >= identity::AudienceKind::Unknown && scope.audience.kind <= identity::AudienceKind::Public, "invalid_enum",
        "scope.audience.kind", "Unknown audience kind.");
    Require(
        scope.participantSource >= identity::SpeakerSource::Unknown && scope.participantSource <= identity::SpeakerSource::ConsentedVoice,
        "invalid_enum", "scope.participantSource", "Unknown participant source.");
    Strings(scope.audience.recipientEntityIds, "scope.audience.recipientEntityIds");
    std::set<std::string> recipients;
    for (const auto& recipient : scope.audience.recipientEntityIds)
    {
        Require(recipients.insert(recipient).second, "duplicate_recipient", "scope.audience.recipientEntityIds",
            "Duplicate recipient identity.");
    }
}

void Deliverable(const agents::DeliverableContract& contract)
{
    Require(contract.requirements.size() <= MaximumItems, "limit_exceeded", "deliverableContract.requirements",
        "Too many deliverable requirements.");
    for (const auto& requirement : contract.requirements)
    {
        Require(requirement.section >= agents::DeliverableSection::Steps &&
                    requirement.section <= agents::DeliverableSection::AcceptanceCriteria,
            "invalid_enum", "deliverableContract.requirements.section", "Unknown deliverable section.");
        Text(requirement.instruction, "deliverableContract.requirements.instruction");
    }
}

void CheckTask(const TaskContract& contract)
{
    Version(contract.version);
    Stamp(contract.stamp, "stamp");
    Scope(contract.scope, contract.stamp);
    Require(contract.goal.find_first_not_of(" \t\r\n") != std::string::npos, "missing_goal", "goal", "Goal is required.");
    Text(contract.goal, "goal");
    Strings(contract.positiveConstraints, "positiveConstraints");
    Strings(contract.negativeConstraints, "negativeConstraints");
    Strings(contract.deliverables, "deliverables", true);
    Require(!contract.acceptanceObligations.empty(), "missing_acceptance", "acceptanceObligations", "Acceptance obligations are required.");
    Strings(contract.acceptanceObligations, "acceptanceObligations", true);
    Deliverable(contract.deliverableContract);
    Require(contract.resourceCeilings.maximumParallel > 0 && contract.resourceCeilings.maximumAttemptsPerNode > 0 &&
                contract.resourceCeilings.maximumRequests > 0 && contract.resourceCeilings.maximumActiveMilliseconds > 0,
        "invalid_budget", "resourceCeilings", "Parallel, attempt, request and active time ceilings must be positive.");
    Stamp(contract.cancellation.origin, "cancellation.origin");
    Require(contract.stamp.SameSession(contract.cancellation.origin) &&
                contract.stamp.policyVersion == contract.cancellation.origin.policyVersion,
        "lineage_mismatch", "cancellation.origin", "Cancellation origin must retain session generation and policy.");
    Strings(contract.cancellation.ancestorTaskIds, "cancellation.ancestorTaskIds");
    std::set<std::string> ancestors;
    for (const auto& ancestor : contract.cancellation.ancestorTaskIds)
    {
        Require(ancestor != contract.stamp.taskId && ancestors.insert(ancestor).second, "invalid_lineage", "cancellation.ancestorTaskIds",
            "Lineage must be acyclic and unique.");
    }
    Text(contract.sourceKind, "sourceKind");
    Text(contract.sourceId, "sourceId");
    Require(
        EncodeTask(contract).dump().size() <= MaximumContractJsonBytes, "limit_exceeded", "task", "Task envelope exceeds the byte limit.");
}

void CheckReference(const EvidenceRef& reference)
{
    Version(reference.version);
    Stamp(reference.stamp, "stamp");
    Scope(reference.scope, reference.stamp);
    Text(reference.id, "id");
    Text(reference.sourceLocator, "sourceLocator");
    Text(reference.mediaType, "mediaType");
    Require(reference.observedAtUnixMs > 0, "missing_observation_time", "observedAtUnixMs", "Original observation time is required.");
    Require(!reference.sourceId.empty(), "missing_source_identity", "sourceId", "Original source identity is required.");
    Text(reference.sourceId, "sourceId");
    Require(reference.digest.size() == 64 &&
                std::all_of(reference.digest.begin(), reference.digest.end(), [](unsigned char value)
                    { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'); }),
        "invalid_digest", "digest", "Evidence requires a 64-character hexadecimal SHA256 digest.");
    Require(EncodeReference(reference).dump().size() <= MaximumContractJsonBytes, "limit_exceeded", "evidence",
        "Evidence envelope exceeds the byte limit.");
}

template <typename Operation> ContractValidation Validate(Operation operation)
{
    try
    {
        operation();
        return Good();
    }
    catch (const ValidationFailure& failure)
    {
        return failure.validation;
    }
    catch (const std::exception& error)
    {
        return Bad("invalid_json", "json", error.what());
    }
}

void Object(const json& object, const std::string& field, std::initializer_list<const char*> keys)
{
    Require(object.is_object(), "invalid_type", field, "An object is required.");
    for (auto entry = object.begin(); entry != object.end(); ++entry)
    {
        Require(std::any_of(keys.begin(), keys.end(), [&](const char* key) { return entry.key() == key; }), "unknown_field",
            field + "." + entry.key(), "Unrecognized fields cannot be silently discarded.");
    }
    for (const auto* key : keys)
    {
        Require(object.contains(key), "missing_field", field + "." + key, "Required field is absent.");
    }
}

std::string ReadText(const json& object, const char* key)
{
    Require(object.at(key).is_string(), "invalid_type", key, "A string is required.");
    auto value = object.at(key).get<std::string>();
    Text(value, key, false);
    return value;
}

template <typename Number> Number ReadNumber(const json& object, const char* key)
{
    const auto& value = object.at(key);
    Require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0), "invalid_number", key,
        "An exact nonnegative integer is required.");
    const auto number = value.get<std::uint64_t>();
    Require(number <= std::numeric_limits<Number>::max(), "invalid_number", key, "Integer exceeds the destination range.");
    return static_cast<Number>(number);
}

bool ReadBool(const json& object, const char* key)
{
    Require(object.at(key).is_boolean(), "invalid_type", key, "A boolean is required.");
    return object.at(key).get<bool>();
}

std::vector<std::string> ReadStrings(const json& object, const char* key)
{
    const auto& array = object.at(key);
    Require(array.is_array(), "invalid_type", key, "An array is required.");
    Require(array.size() <= MaximumItems, "limit_exceeded", key, "Too many array entries.");
    std::vector<std::string> values;
    for (const auto& value : array)
    {
        Require(value.is_string(), "invalid_type", key, "Array entries must be strings.");
        values.push_back(value.get<std::string>());
    }
    Strings(values, key);
    return values;
}

json EncodeVersion(const SchemaVersion& version)
{
    return {{"major", version.major}, {"minor", version.minor}};
}

SchemaVersion DecodeVersion(const json& object)
{
    Object(object, "version", {"major", "minor"});
    SchemaVersion version{ReadNumber<std::uint32_t>(object, "major"), ReadNumber<std::uint32_t>(object, "minor")};
    Version(version);
    return version;
}

json EncodeStamp(const runtime::RuntimeStamp& stamp)
{
    return {{"companionId", stamp.companionId}, {"sessionId", stamp.sessionId}, {"generation", stamp.generation}, {"taskId", stamp.taskId},
        {"attemptId", stamp.attemptId}, {"policyVersion", stamp.policyVersion}};
}

runtime::RuntimeStamp DecodeStamp(const json& object)
{
    Object(object, "stamp", {"companionId", "sessionId", "generation", "taskId", "attemptId", "policyVersion"});
    return {ReadText(object, "companionId"), ReadText(object, "sessionId"), ReadNumber<std::uint64_t>(object, "generation"),
        ReadText(object, "taskId"), ReadText(object, "attemptId"), ReadNumber<std::uint64_t>(object, "policyVersion")};
}

json EncodeScope(const memory::MemoryScope& scope)
{
    return {{"participantId", scope.participantId}, {"companionId", scope.companionId},
        {"participantSource", static_cast<std::uint32_t>(scope.participantSource)}, {"consentRevision", scope.consentRevision},
        {"audience", {{"kind", static_cast<std::uint32_t>(scope.audience.kind)}, {"audienceId", scope.audience.audienceId},
                         {"revision", scope.audience.revision}, {"recipientEntityIds", scope.audience.recipientEntityIds}}}};
}

memory::MemoryScope DecodeScope(const json& object)
{
    Object(object, "scope", {"participantId", "companionId", "participantSource", "consentRevision", "audience"});
    const auto& audience = object.at("audience");
    Object(audience, "scope.audience", {"kind", "audienceId", "revision", "recipientEntityIds"});
    const auto kind = ReadNumber<std::uint32_t>(audience, "kind");
    const auto source = ReadNumber<std::uint32_t>(object, "participantSource");
    Require(kind <= 3 && source <= 3, "invalid_enum", "scope", "Audience or participant source is out of range.");
    memory::MemoryScope scope;
    scope.participantId = ReadText(object, "participantId");
    scope.companionId = ReadText(object, "companionId");
    scope.participantSource = static_cast<identity::SpeakerSource>(source);
    scope.consentRevision = ReadNumber<std::uint64_t>(object, "consentRevision");
    scope.audience = {static_cast<identity::AudienceKind>(kind), ReadText(audience, "audienceId"),
        ReadNumber<std::uint64_t>(audience, "revision"), ReadStrings(audience, "recipientEntityIds")};
    return scope;
}

json EncodeBudget(const agents::WorkflowBudget& budget)
{
    return {{"maximumParallel", budget.maximumParallel}, {"maximumAttemptsPerNode", budget.maximumAttemptsPerNode},
        {"maximumRequests", budget.maximumRequests}, {"maximumActiveMilliseconds", budget.maximumActiveMilliseconds},
        {"maximumReportedTokens", budget.maximumReportedTokens}, {"maximumProviderCalls", budget.maximumProviderCalls},
        {"maximumToolCalls", budget.maximumToolCalls}, {"maximumToolOutputBytes", budget.maximumToolOutputBytes}};
}

agents::WorkflowBudget DecodeBudget(const json& object)
{
    Object(object, "resourceCeilings",
        {"maximumParallel", "maximumAttemptsPerNode", "maximumRequests", "maximumActiveMilliseconds", "maximumReportedTokens",
            "maximumProviderCalls", "maximumToolCalls", "maximumToolOutputBytes"});
    return {ReadNumber<std::uint32_t>(object, "maximumParallel"), ReadNumber<std::uint32_t>(object, "maximumAttemptsPerNode"),
        ReadNumber<std::uint32_t>(object, "maximumRequests"), ReadNumber<std::uint64_t>(object, "maximumActiveMilliseconds"),
        ReadNumber<std::uint64_t>(object, "maximumReportedTokens"), ReadNumber<std::uint32_t>(object, "maximumProviderCalls"),
        ReadNumber<std::uint32_t>(object, "maximumToolCalls"), ReadNumber<std::uint64_t>(object, "maximumToolOutputBytes")};
}

json EncodeDeliverable(const agents::DeliverableContract& contract)
{
    auto requirements = json::array();
    for (const auto& requirement : contract.requirements)
    {
        requirements.push_back({{"section", static_cast<std::uint32_t>(requirement.section)}, {"instruction", requirement.instruction},
            {"allowNoneWithReason", requirement.allowNoneWithReason}});
    }
    return {{"requirements", requirements}, {"requireAllPrerequisites", contract.requireAllPrerequisites}};
}

agents::DeliverableContract DecodeDeliverable(const json& object)
{
    Object(object, "deliverableContract", {"requirements", "requireAllPrerequisites"});
    const auto& requirements = object.at("requirements");
    Require(requirements.is_array(), "invalid_type", "deliverableContract.requirements", "An array is required.");
    Require(requirements.size() <= MaximumItems, "limit_exceeded", "deliverableContract.requirements", "Too many requirements.");
    agents::DeliverableContract contract;
    contract.requireAllPrerequisites = ReadBool(object, "requireAllPrerequisites");
    for (const auto& requirement : requirements)
    {
        Object(requirement, "requirement", {"section", "instruction", "allowNoneWithReason"});
        const auto section = ReadNumber<std::uint32_t>(requirement, "section");
        Require(section <= 3, "invalid_enum", "requirement.section", "Unknown deliverable section.");
        contract.requirements.push_back({static_cast<agents::DeliverableSection>(section), ReadText(requirement, "instruction"),
            ReadBool(requirement, "allowNoneWithReason")});
    }
    return contract;
}

json EncodeTask(const TaskContract& contract)
{
    return {{"version", EncodeVersion(contract.version)}, {"stamp", EncodeStamp(contract.stamp)}, {"scope", EncodeScope(contract.scope)},
        {"goal", contract.goal}, {"positiveConstraints", contract.positiveConstraints},
        {"negativeConstraints", contract.negativeConstraints}, {"deliverables", contract.deliverables},
        {"acceptanceObligations", contract.acceptanceObligations}, {"resourceCeilings", EncodeBudget(contract.resourceCeilings)},
        {"deliverableContract", EncodeDeliverable(contract.deliverableContract)},
        {"cancellation",
            {{"origin", EncodeStamp(contract.cancellation.origin)}, {"ancestorTaskIds", contract.cancellation.ancestorTaskIds}}},
        {"sourceKind", contract.sourceKind}, {"sourceId", contract.sourceId}};
}

TaskContract DecodeTask(const json& object)
{
    Require(object.is_object(), "invalid_type", "task", "An object is required.");
    Require(object.contains("version"), "missing_field", "version", "Schema version is required.");
    TaskContract contract;
    contract.version = DecodeVersion(object.at("version"));
    Require(object.contains("goal"), "missing_goal", "goal", "Goal is required.");
    Require(
        object.contains("acceptanceObligations"), "missing_acceptance", "acceptanceObligations", "Acceptance obligations are required.");
    Object(object, "task",
        {"version", "stamp", "scope", "goal", "positiveConstraints", "negativeConstraints", "deliverables", "acceptanceObligations",
            "resourceCeilings", "deliverableContract", "cancellation", "sourceKind", "sourceId"});
    contract.stamp = DecodeStamp(object.at("stamp"));
    contract.scope = DecodeScope(object.at("scope"));
    contract.goal = ReadText(object, "goal");
    contract.positiveConstraints = ReadStrings(object, "positiveConstraints");
    contract.negativeConstraints = ReadStrings(object, "negativeConstraints");
    contract.deliverables = ReadStrings(object, "deliverables");
    contract.acceptanceObligations = ReadStrings(object, "acceptanceObligations");
    contract.resourceCeilings = DecodeBudget(object.at("resourceCeilings"));
    contract.deliverableContract = DecodeDeliverable(object.at("deliverableContract"));
    const auto& cancellation = object.at("cancellation");
    Object(cancellation, "cancellation", {"origin", "ancestorTaskIds"});
    contract.cancellation = {DecodeStamp(cancellation.at("origin")), ReadStrings(cancellation, "ancestorTaskIds")};
    contract.sourceKind = ReadText(object, "sourceKind");
    contract.sourceId = ReadText(object, "sourceId");
    CheckTask(contract);
    return contract;
}

json EncodeReference(const EvidenceRef& reference)
{
    return {{"version", EncodeVersion(reference.version)}, {"id", reference.id}, {"sourceLocator", reference.sourceLocator},
        {"digest", reference.digest}, {"mediaType", reference.mediaType}, {"observedAtUnixMs", reference.observedAtUnixMs},
        {"sourceId", reference.sourceId}, {"stamp", EncodeStamp(reference.stamp)}, {"scope", EncodeScope(reference.scope)}};
}

EvidenceRef DecodeReference(const json& object)
{
    Object(object, "evidence", {"version", "id", "sourceLocator", "digest", "mediaType", "stamp", "scope", "observedAtUnixMs", "sourceId"});
    EvidenceRef reference{DecodeVersion(object.at("version")), ReadText(object, "id"), ReadText(object, "sourceLocator"),
        ReadText(object, "digest"), ReadText(object, "mediaType"), DecodeStamp(object.at("stamp")), DecodeScope(object.at("scope")),
        ReadNumber<std::uint64_t>(object, "observedAtUnixMs"), ReadText(object, "sourceId")};
    CheckReference(reference);
    return reference;
}

json Parse(std::string_view input)
{
    Require(input.size() <= MaximumContractJsonBytes, "limit_exceeded", "json", "JSON exceeds the byte limit.");
    std::vector<std::set<std::string>> objectKeys;
    return json::parse(input,
        [&](int depth, json::parse_event_t event, json& value)
        {
            Require(depth <= 32, "limit_exceeded", "json", "JSON nesting exceeds the depth limit.");
            if (event == json::parse_event_t::object_start)
            {
                objectKeys.emplace_back();
            }
            else if (event == json::parse_event_t::key)
            {
                Require(!objectKeys.empty() && objectKeys.back().insert(value.get<std::string>()).second, "duplicate_field", "json",
                    "Duplicate object keys are not permitted.");
            }
            else if (event == json::parse_event_t::object_end)
            {
                objectKeys.pop_back();
            }
            return true;
        });
}

void WriteJson(const json& object, std::string& output)
{
    auto serialized = object.dump();
    Require(serialized.size() <= MaximumContractJsonBytes, "limit_exceeded", "json", "Encoded JSON exceeds the byte limit.");
    output = std::move(serialized);
}

ContractValidation GuardCurrent(const runtime::RuntimeStamp& stamp, const CurrentStampGuard& matchesCurrent)
{
    if (!matchesCurrent)
    {
        return Bad("missing_host_guard", "stamp", "Current host admission is required.");
    }
    try
    {
        return matchesCurrent(stamp) ? Good() : Bad("stale_runtime", "stamp", "Host no longer admits this runtime identity.");
    }
    catch (...)
    {
        return Bad("host_guard_failed", "stamp", "Host admission could not be established.");
    }
}

ContractValidation FinishAdapter(TaskContract requirements, TaskContract& output)
{
    const auto validation = ValidateTaskContract(requirements);
    if (validation)
    {
        output = std::move(requirements);
    }
    return validation;
}
}

bool SameRuntimeStamp(const runtime::RuntimeStamp& left, const runtime::RuntimeStamp& right)
{
    return left.SameSession(right) && left.taskId == right.taskId && left.attemptId == right.attemptId &&
           left.policyVersion == right.policyVersion;
}

bool SameMemoryScope(const memory::MemoryScope& left, const memory::MemoryScope& right)
{
    return left.companionId == right.companionId && left.participantId == right.participantId &&
           left.participantSource == right.participantSource && left.consentRevision == right.consentRevision &&
           left.audience.kind == right.audience.kind && left.audience.audienceId == right.audience.audienceId &&
           left.audience.revision == right.audience.revision && left.audience.recipientEntityIds == right.audience.recipientEntityIds;
}

ContractValidation ValidateTaskContract(const TaskContract& contract)
{
    return Validate([&] { CheckTask(contract); });
}

ContractValidation ValidateTaskAdmission(const TaskContract& contract, const runtime::RuntimeStamp& current,
    const memory::MemoryScope& scope, const CurrentStampGuard& matchesCurrent, std::stop_token cancellation)
{
    const auto validation = ValidateTaskContract(contract);
    if (!validation)
    {
        return validation;
    }
    if (cancellation.stop_requested())
    {
        return Bad("cancelled", "cancellation", "The originating operation was cancelled.");
    }
    if (!SameRuntimeStamp(contract.stamp, current))
    {
        return Bad("stamp_mismatch", "stamp", "The contract does not match the current task identity.");
    }
    if (!SameMemoryScope(contract.scope, scope))
    {
        return Bad("scope_mismatch", "scope", "The contract does not match the current disclosure scope.");
    }
    return GuardCurrent(contract.stamp, matchesCurrent);
}

ContractValidation SerializeTaskContract(const TaskContract& contract, std::string& output)
{
    return Validate(
        [&]
        {
            CheckTask(contract);
            WriteJson(EncodeTask(contract), output);
        });
}

ContractValidation DeserializeTaskContract(std::string_view input, TaskContract& output)
{
    return Validate(
        [&]
        {
            auto parsed = DecodeTask(Parse(input));
            output = std::move(parsed);
        });
}

ContractValidation ValidateEvidenceRef(const EvidenceRef& reference)
{
    return Validate([&] { CheckReference(reference); });
}

ContractValidation SerializeEvidenceRef(const EvidenceRef& reference, std::string& output)
{
    return Validate(
        [&]
        {
            CheckReference(reference);
            WriteJson(EncodeReference(reference), output);
        });
}

ContractValidation DeserializeEvidenceRef(std::string_view input, EvidenceRef& output)
{
    return Validate(
        [&]
        {
            auto parsed = DecodeReference(Parse(input));
            output = std::move(parsed);
        });
}

std::optional<EvidenceBundle> EvidenceBundle::Create(std::vector<EvidenceRef> references, runtime::RuntimeStamp stamp,
    memory::MemoryScope scope, ContractValidation& validation, SchemaVersion version)
{
    validation = Validate(
        [&]
        {
            Version(version);
            Stamp(stamp, "stamp");
            Scope(scope, stamp);
            Require(!references.empty(), "missing_evidence", "references", "At least one evidence reference is required.");
            Require(references.size() <= MaximumItems, "limit_exceeded", "references", "Too many evidence references.");
            std::set<std::string> ids;
            for (const auto& reference : references)
            {
                CheckReference(reference);
                Require(SameRuntimeStamp(reference.stamp, stamp), "stamp_mismatch", "references.stamp",
                    "Reference belongs to a different task identity.");
                Require(SameMemoryScope(reference.scope, scope), "scope_mismatch", "references.scope",
                    "Reference belongs to a different disclosure scope.");
                Require(ids.insert(reference.id).second, "duplicate_evidence", "references.id", "Duplicate evidence ID.");
            }
            auto encodedReferences = json::array();
            for (const auto& reference : references)
            {
                encodedReferences.push_back(EncodeReference(reference));
            }
            const json encoded{{"version", EncodeVersion(version)}, {"stamp", EncodeStamp(stamp)}, {"scope", EncodeScope(scope)},
                {"references", encodedReferences}};
            Require(
                encoded.dump().size() <= MaximumContractJsonBytes, "limit_exceeded", "bundle", "Evidence bundle exceeds the byte limit.");
        });
    if (!validation)
    {
        return std::nullopt;
    }
    return EvidenceBundle(std::move(references), std::move(stamp), std::move(scope), version);
}

ContractValidation ValidateEvidenceAdmission(const EvidenceBundle& bundle, const TaskContract& task, const runtime::RuntimeStamp& current,
    const memory::MemoryScope& scope, const CurrentStampGuard& matchesCurrent, std::stop_token cancellation)
{
    const auto validation = ValidateTaskAdmission(task, current, scope, matchesCurrent, cancellation);
    if (!validation)
    {
        return validation;
    }
    if (!SameRuntimeStamp(bundle.stamp(), task.stamp))
    {
        return Bad("stamp_mismatch", "bundle.stamp", "Evidence does not match the admitted task identity.");
    }
    if (!SameMemoryScope(bundle.scope(), task.scope))
    {
        return Bad("scope_mismatch", "bundle.scope", "Evidence does not match the admitted disclosure scope.");
    }
    // Recheck after inspecting the bundle; host cancellation and scope remain host-owned.
    if (cancellation.stop_requested())
    {
        return Bad("cancelled", "cancellation", "The originating operation was cancelled.");
    }
    return GuardCurrent(bundle.stamp(), matchesCurrent);
}

ContractValidation SerializeEvidenceBundle(const EvidenceBundle& bundle, std::string& output)
{
    return Validate(
        [&]
        {
            auto references = json::array();
            for (const auto& reference : bundle.references())
            {
                references.push_back(EncodeReference(reference));
            }
            WriteJson({{"version", EncodeVersion(bundle.version())}, {"stamp", EncodeStamp(bundle.stamp())},
                          {"scope", EncodeScope(bundle.scope())}, {"references", references}},
                output);
        });
}

std::optional<EvidenceBundle> DeserializeEvidenceBundle(std::string_view input, ContractValidation& validation)
{
    std::optional<EvidenceBundle> result;
    validation = Validate(
        [&]
        {
            const auto object = Parse(input);
            Object(object, "bundle", {"version", "stamp", "scope", "references"});
            const auto version = DecodeVersion(object.at("version"));
            const auto stamp = DecodeStamp(object.at("stamp"));
            const auto scope = DecodeScope(object.at("scope"));
            const auto& array = object.at("references");
            Require(array.is_array(), "invalid_type", "references", "An array is required.");
            Require(array.size() <= MaximumItems, "limit_exceeded", "references", "Too many evidence references.");
            std::vector<EvidenceRef> references;
            for (const auto& entry : array)
            {
                references.push_back(DecodeReference(entry));
            }
            ContractValidation created;
            auto bundle = EvidenceBundle::Create(std::move(references), stamp, scope, created, version);
            if (!created)
            {
                throw ValidationFailure{created};
            }
            result.emplace(std::move(*bundle));
        });
    return result;
}

ContractValidation AdaptTurnTask(const runtime::TurnContext& turn, TaskContract requirements, TaskContract& output)
{
    if (turn.Cancelled())
    {
        return Bad("cancelled", "cancellation", "The originating turn was cancelled.");
    }
    requirements.stamp = turn.stamp;
    requirements.goal = turn.request;
    requirements.sourceKind = turn.speaking ? "voice" : "chat";
    requirements.sourceId = std::to_string(turn.turnId);
    return FinishAdapter(std::move(requirements), output);
}

ContractValidation AdaptNodeTask(const agents::NodeRequest& node, TaskContract requirements, TaskContract& output)
{
    requirements.stamp = node.stamp;
    requirements.goal = node.node.objective;
    requirements.deliverableContract = node.node.deliverableContract;
    if (node.node.readOnly)
    {
        requirements.negativeConstraints.push_back("Workflow node permits read-only work only.");
    }
    requirements.sourceKind = "workflow_node";
    requirements.sourceId = node.node.id;
    return FinishAdapter(std::move(requirements), output);
}

ContractValidation AdaptActionTask(const actions::ActionRequest& action, TaskContract requirements, TaskContract& output)
{
    requirements.stamp = action.authorityStamp;
    requirements.sourceKind = "action";
    requirements.sourceId = action.id;
    return FinishAdapter(std::move(requirements), output);
}
}
