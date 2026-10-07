#include "Core/evidenceBundle.h"
#include "Actions/actionTypes.h"
#include "Runtime/turnContext.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace revia;
using nlohmann::json;

namespace
{
void Check(bool value, const std::string& message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

json ReadJson(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    Check(stream.good(), "fixture missing: " + path.string());
    return json::parse(stream);
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void ContractFixtures(const std::filesystem::path& directory)
{
    const auto fixtures = ReadJson(directory / "contracts.json");
    Check(fixtures.size() == 24, "24 independently expected contract fixtures");
    for (const auto& fixture : fixtures)
    {
        core::TaskContract parsed;
        parsed.goal = "unchanged on failure";
        const auto validation = core::DeserializeTaskContract(fixture.at("contract").dump(), parsed);
        const auto expected = fixture.at("error").get<std::string>();
        Check(validation.valid == expected.empty(), fixture.at("name").get<std::string>() + ": " + validation.code);
        if (!expected.empty())
        {
            Check(validation.code == expected, "named error: " + expected + " got " + validation.code);
            Check(parsed.goal == "unchanged on failure", "failed decode must not overwrite output");
            continue;
        }
        std::string encoded;
        Check(core::SerializeTaskContract(parsed, encoded).valid, "valid fixture serialization");
        Check(json::parse(encoded) == fixture.at("contract"), "independent round-trip expectation");
    }
}

core::TaskContract Task(const std::filesystem::path& directory)
{
    core::TaskContract task;
    Check(core::DeserializeTaskContract(ReadJson(directory / "contracts.json").at(0).at("contract").dump(), task).valid,
        "valid chat contract");
    return task;
}

core::EvidenceRef Reference(const core::TaskContract& task)
{
    return {{1, 0}, "receipt-42", "journal://session-a/receipt-42", std::string(64, 'a'), "application/json", task.stamp, task.scope};
}

void StrictNumbersAndBounds(const std::filesystem::path& directory)
{
    const auto base = ReadJson(directory / "contracts.json").at(0).at("contract");
    for (const json& invalid : {json(-1), json(2.5), json("1"), json(true), json(18446744073709551616.0)})
    {
        auto sample = base;
        sample["stamp"]["generation"] = invalid;
        core::TaskContract parsed;
        Check(core::DeserializeTaskContract(sample.dump(), parsed).code == "invalid_number", "stamp numeric coercion rejected");
    }
    for (const auto* field : {"major", "minor"})
    {
        auto sample = base;
        sample["version"][field] = 4294967296ULL;
        core::TaskContract parsed;
        Check(core::DeserializeTaskContract(sample.dump(), parsed).code == "invalid_number", "version narrowing rejected");
    }
    auto sample = base;
    sample["stamp"]["generation"] = std::numeric_limits<std::uint64_t>::max();
    sample["cancellation"]["origin"]["generation"] = std::numeric_limits<std::uint64_t>::max();
    core::TaskContract parsed;
    Check(core::DeserializeTaskContract(sample.dump(), parsed).valid, "exact uint64 maximum retained");
    Check(parsed.stamp.generation == std::numeric_limits<std::uint64_t>::max(), "no uint64 rounding");
    std::string encoded;
    Check(core::SerializeTaskContract(parsed, encoded).valid && json::parse(encoded) == sample, "uint64 maximum round trip");
    sample = base;
    sample["positiveConstraints"].push_back(std::string(8193, 'x'));
    Check(core::DeserializeTaskContract(sample.dump(), parsed).code == "limit_exceeded", "string limit");
    sample = base;
    sample["resourceCeilings"]["maximumParallel"] = 4294967296ULL;
    Check(core::DeserializeTaskContract(sample.dump(), parsed).code == "invalid_number", "budget narrowing rejected");
    sample = base;
    sample["resourceCeilings"]["maximumToolCalls"] = -1;
    Check(core::DeserializeTaskContract(sample.dump(), parsed).code == "invalid_number", "negative budget rejected");
    sample = base;
    sample["unexpectedObligation"] = "must not silently drop";
    Check(core::DeserializeTaskContract(sample.dump(), parsed).code == "unknown_field", "unknown semantic fields rejected");
    Check(core::DeserializeTaskContract(std::string(core::MaximumContractJsonBytes + 1, ' '), parsed).code == "limit_exceeded",
        "input bound");
    Check(core::DeserializeTaskContract("{broken", parsed).code == "invalid_json", "malformed input");
}

void EvidenceAdmission(const std::filesystem::path& directory, const std::filesystem::path& outputDirectory)
{
    static_assert(!std::is_copy_assignable_v<core::EvidenceBundle>);
    static_assert(std::is_same_v<decltype(std::declval<const core::EvidenceBundle>().references()), const std::vector<core::EvidenceRef>&>);
    const auto task = Task(directory);
    auto reference = Reference(task);
    core::ContractValidation result;
    auto valid = core::EvidenceBundle::Create({reference}, task.stamp, task.scope, result);
    Check(valid.has_value() && result.valid, "valid evidence bundle");
    std::string encoded;
    Check(core::SerializeEvidenceBundle(*valid, encoded).valid, "bundle encode");
    auto decoded = core::DeserializeEvidenceBundle(encoded, result);
    Check(decoded.has_value() && core::SameRuntimeStamp(decoded->references().at(0).stamp, task.stamp), "bundle identity round trip");
    Check(core::SameMemoryScope(decoded->scope(), task.scope), "bundle scope round trip");
    std::string refEncoded;
    Check(core::SerializeEvidenceRef(reference, refEncoded).valid, "reference encode");
    core::EvidenceRef refDecoded;
    Check(core::DeserializeEvidenceRef(refEncoded, refDecoded).valid && refDecoded.digest == std::string(64, 'a'), "reference decode");
    reference.digest = "not-a-sha256";
    Check(core::ValidateEvidenceRef(reference).code == "invalid_digest", "invalid digest rejected");
    reference = Reference(task);
    Check(!core::EvidenceBundle::Create({reference, reference}, task.stamp, task.scope, result) && result.code == "duplicate_evidence",
        "duplicate references");
    reference.stamp.attemptId = "other-attempt";
    Check(!core::EvidenceBundle::Create({reference}, task.stamp, task.scope, result) && result.code == "stamp_mismatch",
        "reference mismatch");
    reference = Reference(task);
    reference.version.major = 2;
    Check(core::ValidateEvidenceRef(reference).code == "unsupported_major", "reference major rejected");

    const auto memoryPath = outputDirectory / "memory.txt";
    {
        std::ofstream memory(memoryPath, std::ios::binary);
        memory << "existing host-owned memory\n";
    }
    const auto before = ReadText(memoryPath);
    std::vector<std::string> events;
    std::string response;
    auto consume = [&](const core::EvidenceBundle& bundle, const core::CurrentStampGuard& guard)
    {
        const auto admission = core::ValidateEvidenceAdmission(bundle, task, task.stamp, task.scope, guard);
        if (!admission)
        {
            events.push_back(admission.code);
            return false;
        }
        response += bundle.references().at(0).sourceLocator;
        std::ofstream memory(memoryPath, std::ios::app);
        memory << bundle.references().at(0).sourceLocator;
        return true;
    };
    auto current = [&](const runtime::RuntimeStamp& stamp) { return core::SameRuntimeStamp(stamp, task.stamp); };
    for (int index = 0; index < 100; ++index)
    {
        auto stamp = task.stamp;
        auto scope = task.scope;
        if (index < 50)
        {
            switch (index % 6)
            {
            case 0:
                stamp.generation = task.stamp.generation - 1;
                break;
            case 1:
                stamp.sessionId += std::to_string(index);
                break;
            case 2:
                stamp.taskId += std::to_string(index);
                break;
            case 3:
                stamp.attemptId += std::to_string(index);
                break;
            case 4:
                stamp.policyVersion += index + 1;
                break;
            case 5:
                stamp.companionId += std::to_string(index);
                scope.companionId = stamp.companionId;
                break;
            }
        }
        else
        {
            switch (index % 8)
            {
            case 0:
                scope.audience.kind = identity::AudienceKind::Public;
                break;
            case 1:
                scope.audience.audienceId += std::to_string(index);
                break;
            case 2:
                scope.audience.revision += index + 1;
                break;
            case 3:
                scope.audience.recipientEntityIds.push_back("other-recipient");
                break;
            case 4:
                scope.participantId += std::to_string(index);
                break;
            case 5:
                scope.participantSource = identity::SpeakerSource::ConsentedVoice;
                break;
            case 6:
                scope.consentRevision += index + 1;
                break;
            case 7:
                scope.audience.recipientEntityIds = {"other-recipient"};
                break;
            }
        }
        reference = Reference(task);
        reference.stamp = stamp;
        reference.scope = scope;
        reference.id += std::to_string(index);
        auto bundle = core::EvidenceBundle::Create({reference}, stamp, scope, result);
        Check(bundle.has_value(), "well-formed stale/cross-audience sample " + std::to_string(index) + " " + result.code);
        Check(!consume(*bundle, current), "invalid evidence admission " + std::to_string(index));
        Check(events.back() == (index < 50 ? "stamp_mismatch" : "scope_mismatch"), "recorded exact rejection reason");
    }
    Check(events.size() == 100 && response.empty(), "100 rejected deliveries cannot reach response");
    Check(ReadText(memoryPath) == before, "100 rejected deliveries cannot change persisted memory");
    Check(core::ValidateEvidenceAdmission(*valid, task, task.stamp, task.scope, {}).code == "missing_host_guard", "guard mandatory");
    Check(core::ValidateEvidenceAdmission(*valid, task, task.stamp, task.scope, [](const auto&) { return false; }).code == "stale_runtime",
        "host can revoke current generation");
    Check(core::ValidateEvidenceAdmission(*valid, task, task.stamp, task.scope,
              [](const auto&) -> bool
              {
                  throw std::runtime_error("host unavailable");
              }).code == "host_guard_failed",
        "host error fails closed");
    std::stop_source stopped;
    stopped.request_stop();
    Check(core::ValidateEvidenceAdmission(*valid, task, task.stamp, task.scope, current, stopped.get_token()).code == "cancelled",
        "cancelled admission");
    Check(consume(*valid, current), "valid current evidence reaches consumer");
    Check(!response.empty() && ReadText(memoryPath) != before, "consumer positive control proves memory and response path active");
}

void LegacyAdapters(const std::filesystem::path& directory)
{
    auto requirements = Task(directory);
    core::TaskContract output;
    runtime::TurnContext turn{"original host request", {}, 42, true, requirements.stamp};
    Check(core::AdaptTurnTask(turn, requirements, output).valid, "turn adapter");
    Check(output.goal == turn.request && core::SameRuntimeStamp(output.stamp, turn.stamp), "original turn identity/request");
    Check(output.acceptanceObligations == requirements.acceptanceObligations &&
              output.negativeConstraints == requirements.negativeConstraints,
        "turn obligations preserved");
    std::stop_source stopped;
    stopped.request_stop();
    runtime::TurnContext cancelled{turn.request, stopped.get_token(), 42, true, requirements.stamp};
    Check(core::AdaptTurnTask(cancelled, requirements, output).code == "cancelled", "cancelled turn adapter");
    agents::NodeRequest node;
    node.stamp = requirements.stamp;
    node.node.id = "node-a";
    node.node.objective = "node objective";
    node.node.deliverableContract.requirements.push_back({agents::DeliverableSection::AcceptanceCriteria, "independent review", false});
    Check(core::AdaptNodeTask(node, requirements, output).valid && output.sourceId == "node-a", "node identity provenance");
    Check(output.deliverableContract.requirements.at(0).instruction == "independent review", "node deliverable retained");
    Check(std::find(output.negativeConstraints.begin(), output.negativeConstraints.end(), "Workflow node permits read-only work only.") !=
              output.negativeConstraints.end(),
        "read-only node restriction retained");
    actions::ActionRequest action;
    action.id = "action-a";
    action.authorityStamp = requirements.stamp;
    Check(core::AdaptActionTask(action, requirements, output).valid && output.sourceId == "action-a", "action identity provenance");
    Check(core::SameRuntimeStamp(output.cancellation.origin, requirements.cancellation.origin), "cancellation lineage retained");
    requirements.acceptanceObligations.clear();
    Check(core::AdaptActionTask(action, requirements, output).code == "missing_acceptance", "adapter cannot supply default obligations");
}

void AdditionalBoundaryChecks(const std::filesystem::path& directory)
{
    auto task = Task(directory);
    task.goal = std::string("bad-") + static_cast<char>(0xff);
    Check(core::ValidateTaskContract(task).code == "invalid_utf8", "raw invalid UTF8 cannot reach consumer");
    task = Task(directory);
    task.positiveConstraints.assign(64, std::string(8192, 'x'));
    Check(core::ValidateTaskContract(task).code == "limit_exceeded", "total raw envelope bound");
    auto reference = Reference(Task(directory));
    reference.scope.audience.recipientEntityIds.clear();
    for (int index = 0; index < 64; ++index)
    {
        reference.scope.audience.recipientEntityIds.push_back(std::to_string(index) + std::string(8190, 'r'));
    }
    Check(core::ValidateEvidenceRef(reference).code == "limit_exceeded", "total raw reference envelope bound");
    const auto fixture = ReadJson(directory / "contracts.json").at(0).at("contract");
    auto sample = fixture;
    sample["version"]["minor"] = 1;
    Check(core::DeserializeTaskContract(sample.dump(), task).code == "unsupported_minor", "future minor cannot discard fields");
    sample = fixture;
    sample["cancellation"]["origin"]["generation"] = 8;
    Check(core::DeserializeTaskContract(sample.dump(), task).code == "lineage_mismatch", "cancellation generation preserved");
    sample = fixture;
    sample["cancellation"]["ancestorTaskIds"] = {"task-42"};
    Check(core::DeserializeTaskContract(sample.dump(), task).code == "invalid_lineage", "cancellation cycle rejected");
    sample = fixture;
    sample["scope"]["audience"]["recipientEntityIds"] = {"owner-a", "owner-a"};
    Check(core::DeserializeTaskContract(sample.dump(), task).code == "duplicate_recipient", "recipient identities must remain distinct");
    sample = fixture;
    sample["scope"]["audience"]["kind"] = 4;
    Check(core::DeserializeTaskContract(sample.dump(), task).code == "invalid_enum", "unknown audience enum rejected");
    sample = fixture;
    sample["positiveConstraints"] = json::array();
    for (int index = 0; index < 65; ++index)
    {
        sample["positiveConstraints"].push_back("bounded condition");
    }
    Check(core::DeserializeTaskContract(sample.dump(), task).code == "limit_exceeded", "array bound");
    Check(core::DeserializeTaskContract("{\"version\":{\"major\":1,\"major\":2,\"minor\":0}}", task).code == "duplicate_field",
        "duplicate field fails before last-wins identity");
    auto nested = std::string(40, '[') + "0" + std::string(40, ']');
    Check(core::DeserializeTaskContract(nested, task).code == "limit_exceeded", "JSON nesting bound");
    task = Task(directory);
    std::string output = "preserved output";
    task.goal.clear();
    Check(!core::SerializeTaskContract(task, output) && output == "preserved output", "serialization failure preserves output");
    reference = Reference(Task(directory));
    core::ContractValidation result;
    auto versionRejected = core::EvidenceBundle::Create({reference}, reference.stamp, reference.scope, result, {2, 0});
    Check(!versionRejected && result.code == "unsupported_major", "bundle major rejected");
    auto empty = core::EvidenceBundle::Create({}, reference.stamp, reference.scope, result);
    Check(!empty && result.code == "missing_evidence", "empty evidence rejected");
    auto scope = reference.scope;
    scope.audience.revision++;
    auto mismatch = core::EvidenceBundle::Create({reference}, reference.stamp, scope, result);
    Check(!mismatch && result.code == "scope_mismatch", "mixed reference scope rejected");
    auto decoded = core::DeserializeEvidenceBundle("{broken", result);
    Check(!decoded && result.code == "invalid_json", "malformed bundle returns named error");
}
}

int main(int argc, char** argv)
{
    try
    {
        const std::filesystem::path fixtures = argc > 1 ? argv[1] : "Tests/Fixtures/TaskContracts";
        const auto outputDirectory =
            std::filesystem::temp_directory_path() /
            ("revia-contract-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(outputDirectory);
        ContractFixtures(fixtures);
        StrictNumbersAndBounds(fixtures);
        EvidenceAdmission(fixtures, outputDirectory);
        LegacyAdapters(fixtures);
        AdditionalBoundaryChecks(fixtures);
        std::filesystem::remove_all(outputDirectory);
        std::cout << "PASS: 24 independent contract fixtures; 100 rejected evidence deliveries with response/memory controls; "
                     "numeric/bounds/adapters\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
