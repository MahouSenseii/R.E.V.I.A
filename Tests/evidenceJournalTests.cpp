#include "Audit/evidenceJournal.h"
#include "Audit/actionAuditLogger.h"
#include "Audit/contentDigest.h"

#include <fstream>
#include <future>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace
{
using namespace revia::audit;
void Check(bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}
struct Directory
{
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("revia-journal-" + NewJournalEventId());
    Directory()
    {
        std::filesystem::create_directories(root);
    }
    ~Directory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
JournalEvent Event(const std::string& id, JournalKind kind = JournalKind::Observation, const std::string& transaction = {})
{
    JournalEvent event;
    event.evidence.id = id;
    event.evidence.sourceLocator = "fixture://read-only-source";
    event.evidence.digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    event.evidence.mediaType = "text/plain";
    event.evidence.observedAtUnixMs = 1791342000123;
    event.evidence.sourceId = "native-observation-fixture";
    event.evidence.stamp = {"companion-fixture", "session-fixture", 7, "task-fixture", "attempt-fixture", 3};
    event.evidence.scope.companionId = "companion-fixture";
    event.evidence.scope.participantId = "owner-fixture";
    event.evidence.scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    event.evidence.scope.consentRevision = 2;
    event.evidence.scope.audience = {revia::identity::AudienceKind::Private, "private-owner", 5, {"owner-fixture"}};
    event.kind = kind;
    event.transactionId = transaction;
    event.redactedSummary = "Source content omitted; locator and digest retained.";
    return event;
}
EvidenceQuery Query()
{
    const auto event = Event("query");
    return {event.evidence.stamp, event.evidence.scope, {}, {}};
}
void TestDependentTaskRefusesUnknownEffects()
{
    Directory directory;
    EvidenceJournal journal(directory.root / "dependencies.jsonl");
    auto intent = Event("dependent-intent", JournalKind::Intent, "operation-1");
    Check(journal.Append(intent).Durable(), "Dependent intent was not retained.");
    auto retry = intent.evidence.stamp;
    retry.attemptId = "new-attempt";
    Check(journal.HasUnresolvedForTask(retry, intent.evidence.scope), "A retry bypassed an unknown prior effect.");
    retry.policyVersion += 1;
    Check(journal.HasUnresolvedForTask(retry, intent.evidence.scope), "Policy refresh bypassed an unknown prior effect.");
    auto other = retry;
    other.taskId = "unrelated-task";
    Check(!journal.HasUnresolvedForTask(other, intent.evidence.scope), "Unrelated task was blocked by global health.");
    auto otherScope = intent.evidence.scope;
    otherScope.consentRevision += 1;
    Check(!journal.HasUnresolvedForTask(retry, otherScope), "Journal assigned uncertainty to a different scope.");
    auto result = Event("wrong-result", JournalKind::Result, "operation-1");
    result.evidence.scope = otherScope;
    Check(journal.Append(result).Durable(), "Other scope result was not retained.");
    Check(journal.HasUnresolvedForTask(retry, intent.evidence.scope), "A foreign scope result resolved the original intent.");
    result.evidence.scope = intent.evidence.scope;
    result.evidence.id = "dependent-result";
    Check(journal.Append(result).Durable(), "Dependent result was not retained.");
    Check(!journal.HasUnresolvedForTask(retry, intent.evidence.scope), "Durable matching result did not resolve uncertainty.");
    EvidenceJournal unreadable(directory.root);
    Check(unreadable.HasUnresolvedForTask(retry, intent.evidence.scope), "Unreadable persistence allowed dependent effects.");
}
std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::vector<nlohmann::json> Decode(const std::string& bytes)
{
    std::vector<nlohmann::json> records;
    std::size_t start = 0;
    while (true)
    {
        const auto end = bytes.find('\n', start);
        if (end == std::string::npos)
            break;
        const auto record = nlohmann::json::parse(bytes.substr(start, end - start), nullptr, false);
        if (!record.is_discarded())
            records.push_back(record);
        start = end + 1;
    }
    return records;
}
void TestTelemetryProducerDoesNotWaitForFailedDiskFlush()
{
    Directory directory;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::atomic<bool> blocked{false};
    EvidenceJournal journal(directory.root / "concurrent-telemetry.jsonl", 2,
        [&](JournalIoBoundary boundary)
        {
            if (boundary == JournalIoBoundary::BeforeFlush && !blocked.exchange(true))
            {
                entered.set_value();
                releaseFuture.wait();
                return false;
            }
            return true;
        });
    Check(journal.QueueTelemetry(Event("old-0")) && journal.QueueTelemetry(Event("old-1")), "Initial telemetry was not admitted.");
    Check(!journal.QueueTelemetry(Event("old-drop")), "Initial bounded queue did not drop overflow.");
    auto flusher = std::async(std::launch::async, [&] { return journal.FlushTelemetry(); });
    const bool reached = enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto producer = std::async(std::launch::async,
        [&]
        {
            return std::vector<bool>{
                journal.QueueTelemetry(Event("new-0")), journal.QueueTelemetry(Event("new-1")), journal.QueueTelemetry(Event("new-drop"))};
        });
    const bool responsive = producer.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    release.set_value();
    const auto admissions = producer.get();
    const bool flushed = flusher.get();
    Check(reached && responsive, "Telemetry producer waited for the blocked disk flush.");
    Check(admissions == std::vector<bool>{true, true, false} && !flushed, "Concurrent queue or injected flush failure was not observed.");
    auto health = journal.Health();
    Check(health.droppedTelemetry == 4 && health.pendingDroppedTelemetry == 4 && health.durableDroppedTelemetry == 0,
        "Failed-batch restoration lost pending drops or exceeded the bounded queue.");
    Check(journal.FlushTelemetry(), "Restored telemetry did not flush after disk recovery.");
    health = journal.Health();
    Check(health.droppedTelemetry == 4 && health.pendingDroppedTelemetry == 0 && health.durableDroppedTelemetry == 4,
        "Durable dropped marker was counted twice or left pending.");
    const auto records = Decode(Bytes(journal.Path()));
    Check(records[records.size() - 2].at("event_id") == "old-0" && records.back().at("event_id") == "old-1",
        "Failure restoration discarded accepted older telemetry instead of recording new overflow.");
}

void TestDurableDropMarkerIsNotRestoredAfterLaterFailure()
{
    Directory directory;
    std::size_t writes = 0;
    EvidenceJournal journal(directory.root / "drop-acknowledgement.jsonl", 2,
        [&](JournalIoBoundary boundary) { return boundary != JournalIoBoundary::BeforeWrite || ++writes != 2; });
    Check(journal.QueueTelemetry(Event("retained-0")) && journal.QueueTelemetry(Event("retained-1")), "Marker fixture queue failed.");
    Check(!journal.QueueTelemetry(Event("first-drop")) && !journal.FlushTelemetry(), "Post-marker event failure was not injected.");
    auto health = journal.Health();
    Check(health.durableDroppedTelemetry == 1 && health.pendingDroppedTelemetry == 0 && health.droppedTelemetry == 1,
        "Already durable drop marker was restored as pending.");
    Check(!journal.QueueTelemetry(Event("second-drop")), "Restored queue exceeded capacity.");
    health = journal.Recover();
    Check(health.durableDroppedTelemetry == 1 && health.pendingDroppedTelemetry == 1 && health.droppedTelemetry == 2,
        "Recovery hid newly pending telemetry drops.");
    Check(journal.FlushTelemetry(), "Marker acknowledgement fixture did not recover.");
    Check(journal.Health().durableDroppedTelemetry == 2 && journal.Health().pendingDroppedTelemetry == 0,
        "Retry duplicated a previously durable drop marker.");
}
void TestReplayDeduplicatesAndQuarantinesTwentyCrashTails()
{
    Directory directory;
    const auto baseline = directory.root / "baseline.jsonl";
    EvidenceJournal writer(baseline);
    for (int index = 0; index < 50; ++index)
        Check(writer.Append(Event("known-" + std::to_string(index))).Durable(), "Known event did not persist.");
    Check(writer.Append(Event("known-9")).state == AppendState::Duplicate, "Exact redelivery was not deduplicated.");
    const std::string bytes = Bytes(baseline);
    const auto oracle = Decode(bytes);
    Check(oracle.front().at("evidence").at("observedAtUnixMs") == 1791342000123 &&
              oracle.front().at("evidence").at("sourceId") == "native-observation-fixture",
        "Journal replaced original observation metadata.");
    Check(oracle.size() == 50, "Independent decoder found a duplicate or missing committed record.");
    auto conflict = Event("known-9");
    conflict.evidence.digest.assign(64, 'a');
    Check(
        writer.Append(conflict).state == AppendState::Rejected && Bytes(baseline) == bytes, "Different content reused an event identity.");
    const auto candidate = oracle.back().dump();
    for (int crash = 0; crash < 20; ++crash)
    {
        const auto path = directory.root / ("crash-" + std::to_string(crash) + ".jsonl");
        const auto cut = 1 + (candidate.size() - 2) * crash / 19;
        const std::string fragment = candidate.substr(0, cut);
        {
            std::ofstream output(path, std::ios::binary);
            output << bytes << oracle[9].dump() << '\n' << fragment;
        }
        EvidenceJournal restarted(path);
        for (int replay = 0; replay < 2; ++replay)
        {
            const auto health = restarted.Recover();
            const auto references = restarted.Read(Query());
            Check(health.quarantinedRecords == 1 && health.degraded && references.size() == 50,
                "Crash tail was hidden or duplicate delivery changed recovery.");
            for (int index = 0; index < 50; ++index)
                Check(references[index].id == "known-" + std::to_string(index), "Committed projection order changed.");
        }
        Check(Bytes(path) == bytes + oracle[9].dump() + '\n' + fragment, "Read-only recovery rewrote historical bytes.");
        Check(restarted.Append(Event("after-crash")).Durable(), "Quarantined tail prevented later valid append.");
        const auto repaired = Bytes(path);
        Check(repaired.starts_with(bytes + oracle[9].dump() + '\n' + fragment + '\n'), "Repair discarded torn bytes.");
        const auto decoded = Decode(repaired);
        Check(decoded[decoded.size() - 2].at("record_type") == "recovery", "Recovery marker missing from disk.");
        Check(restarted.Read(Query()).size() == 51, "Valid post-crash event did not project.");
    }
}
void TestExactScopeAndStampAdmission()
{
    Directory directory;
    EvidenceJournal journal(directory.root / "scope.jsonl");
    Check(journal.Append(Event("private-record")).Durable(), "Scoped evidence did not persist.");
    for (int index = 0; index < 100; ++index)
    {
        auto query = Query();
        switch (index % 10)
        {
        case 0:
            query.stamp.companionId = "different";
            break;
        case 1:
            query.stamp.sessionId = "different";
            break;
        case 2:
            ++query.stamp.generation;
            break;
        case 3:
            query.stamp.taskId = "different";
            break;
        case 4:
            query.stamp.attemptId = "different";
            break;
        case 5:
            ++query.stamp.policyVersion;
            break;
        case 6:
            query.scope.participantId = "different";
            break;
        case 7:
            ++query.scope.audience.revision;
            break;
        case 8:
            query.scope.audience.recipientEntityIds = {"different"};
            break;
        case 9:
            ++query.scope.consentRevision;
            break;
        }
        Check(journal.Read(query).empty(), "Evidence crossed an exact scope/stamp boundary.");
    }
    Check(journal.Read(Query()).size() == 1, "Current captured scope lost its evidence.");
    auto invalid = Event("invalid");
    invalid.evidence.digest = "not-sha256";
    Check(journal.Append(invalid).state == AppendState::Rejected, "Invalid evidence was persisted.");
}
void TestOneHundredDeniedIntentsAndBoundedTelemetry()
{
    Directory directory;
    bool denied = true;
    EvidenceJournal journal(directory.root / "pressure.jsonl", 2, [&](JournalIoBoundary) { return !denied; });
    int executorCalls = 0;
    for (int index = 0; index < 100; ++index)
    {
        (void)journal.QueueTelemetry(Event("telemetry-" + std::to_string(index)));
        const auto receipt = journal.Append(Event("intent-" + std::to_string(index), JournalKind::Intent, "tx-" + std::to_string(index)));
        if (receipt.Durable())
            ++executorCalls;
        Check(receipt.state == AppendState::Degraded && !receipt.error.empty(), "Denied persistence was not surfaced.");
    }
    Check(executorCalls == 0 && journal.Health().droppedTelemetry == 98, "Denied intent dispatched or queue was unbounded.");
    Check(!journal.QueueTelemetry(Event("buffered-intent", JournalKind::Intent, "critical")), "Critical intent entered telemetry queue.");
    denied = false;
    Check(journal.Append(Event("admitted-intent", JournalKind::Intent, "admitted")).Durable(),
        "Telemetry saturation blocked durable intent.");
    Check(journal.FlushTelemetry(), "Buffered telemetry did not flush after persistence recovered.");
    const auto decoded = Decode(Bytes(journal.Path()));
    Check(decoded.size() == 4 && decoded[0].at("event_id") == "admitted-intent", "Telemetry occupied foreground admission path.");
    Check(decoded[1].at("dropped_count") == 98, "Dropped-count marker missing or inaccurate.");
    Check(decoded[2].at("event_id") == "telemetry-0" && decoded[3].at("event_id") == "telemetry-1",
        "Bounded telemetry did not preserve accepted order.");
}
void TestSixtyRealEffectsSurviveFailedResultFlushAndReadOnlyRestart()
{
    static const char* expectedDigests[] = {
        "a7cb5eee6442a7f46785ce693b1a90d5e5d9b25f40cda7e528140c49a57ea43f",
        "b21832c53c457d15142be63a076b726fcc3c7b9ed8a128dcbb92d17675967994",
        "faa0e101262d2b502e5f12741446e752e91b293fda13ee4fa13e563c061a1ef1",
        "cc3d2a7f89a08d24891a6c39910232cafc66e115354121c0038ee7c875e09e71",
        "719c77491fcca0a2783ad0d16975732e4e6b3cab3900220e76abb9a7f03f3ddf",
        "995936fb69acd4fdc6228dba488def496c36bf18971756ea7eeb65e858526379",
        "de34a941d6621d5b7bc628b3d3588b54a239bed58e87633aac8ec385748bc7aa",
        "e9c732711b2ba118b293ad4f9268f02daadc2e862c60f17c9906d8faf3de8171",
        "03d98ae37cfab9571f18b3f31c7ed1ee448c8eb0abef44237ecfc3496d33b3e7",
        "974f295100c13208c9413d436fab326d3591c4bf714922f4894914076959641e",
        "9fe2debd82dcaa4413ca89e8845f5bcdf247cf2e395fdeeb4ed64741a86817e5",
        "209b0c089aaa3f4760d5061114cae9e6844eccbb30bb17d7442798621ccd2196",
        "df1c1bb9e92538b74e71b60b51e3b5eaafcf337a884e04d56f8b7b87ef16cb52",
        "2c39ece0cace83fcfb0ef8fe5a709a75a08e6870262d989688840a0f14bf05d7",
        "ec86ceb8b97880123347a76ca5a806e4afb3176ea09c19b564fc131144590421",
        "244f17c5fa28661e4457a6dcd09e27e17e6c3019426fd04df03ba2af7464b791",
        "fd8971bbe00ec115f3b04fc0eb66add2090f84fc9997ee3f1bd7ed296038efa7",
        "0335b2691d6cb554f0ce0faffde1a0a1ac227ce74a2e0b503b9c1bfb3124d62f",
        "5098a2fa5ec7c832544ef2ea154d4ea828c0eb2b278435a6125d3944f450ddd6",
        "cbb131d21f5d1c5d27607773566f80802994611fbca4715448ccfa318d4b9544",
        "2c8f19be8b52ae8ec843082b8d0a9ca44f43136ab50050448ef0974ba9bc1c8b",
        "8a5e4091a1710c26bb2f4ffe1a9fa2c1a051af9e2e3ef1113dbb6072b28d7194",
        "a8d70e9d16dc0b9dfc7cb0e7e7e018d65024d6ca9f49aaa640a84b20903e925f",
        "6d84499423ec3401c4ab83f4c891e0651906893c98c65fa6165bd20ecdd3c92e",
        "51dc6aec6a84e7e2e5cb3c0c6517777f20f2bc5e4a5697d38f1108b8839473ce",
        "ef0eed97f5659fcaa8273d11b274b80c6669e4703878ca835d3ee7992b7d191a",
        "2a11c30977d8fcd889598848e013c955ab5108a446f602176cde430a36d608b7",
        "da2a39109061c87ae5f4dfd45f56500edcee2ece614d97858d0dcf59b82d114a",
        "47310df2822a4f91360e633ec4766c3dd354291a617029d60f4a951a10f563a5",
        "5e43e360c15535994a9db85f41541122fc0ab5cbff76ca641aa89cad8b19298c",
        "5003c41d437b3f79dc0e7eab6be1fa6a485c155fa3cc05b96eedf585f7195b5b",
        "b834d8a5839e5de96385d871847b3cadce0f093516561eeaf4faf7ffe9f09f54",
        "d966257d96a878fe18f5795fbe10c56dbb6d993a0e6f0569e97a04b57b840396",
        "dc3b38d7bbb25105d08819898616d338c931a89a5e9947cdfd1a5b787ceef05c",
        "6c1f7c04b77ca78bed763873d7db8158c60454330b95de8b1d5ceb3bd1b8b109",
        "d7713646362f5d559acb78961d41d6943a4893f80b1f8020a9a0778a87d399a9",
        "920df517b00da66745815eee977fc935ffd7145bbacf31cc23b242fcb691b6c6",
        "28280e478f604113d99e7ea3c3dd3b888b0c536d1aea75df29758cfb57f11ff7",
        "7b01fe7db97728aea8e8faa3c40d4033f38b5283afc6220de996f9025158028f",
        "b1ee569eea62fcecbfe9c090ed310dfd34122bf3e010d29ce7b2f8fda1304cb5",
        "cf02366f78af443865a6be6d2461feaf0154b6810862f1e012eb0a5b70bb27a4",
        "8c49496b2e816f43ed950c38cb098b7d47b980b0c81c12629fd6170eb2371f07",
        "edd96bfb91efcb61c91cb63d4f711cbead55c64971551de81afd5e64e5e60db7",
        "dca8f3615ba02542649d55a8b48d8babf47ae759651f608884b73a3eef8f05e2",
        "96cd4141d827dd4daea03358f8e4404b5081c901b6054df0ee4dc0e51f72cb8e",
        "fd56c8900a6a4b05bcc35aba5af722238ec60638e6448062136fd306ca194982",
        "8deb4e6712bb0ef9aefafc8b158f3992be6796cf83764f17c322316077867d6e",
        "918da3b45aabe594d655c4337e7b87c356eb2e23dc5a08a0632a867cade03239",
        "d192b26b2aaa3931329c6cea675a2437166c1f976609f64504cc4aa7db5d11b9",
        "d28f758677867192a25f7271d86d66842115156c2c524d5821d5eb3fcf580d25",
        "cb3813ce08bc6cc21911d65c8f9938fce2208f531bb0d43884297e1cb501c914",
        "5eed2010d5e8f8d35089bcc5593c0ab542fc4b534a9c4873721ce68656c3dd1e",
        "5a330bbddecc72cea5a95fa5aee5a041a4c726b6bf5bf34dab8b4edf05e9f77d",
        "652178ecc62c6b7af9396143d5f4222cdbea856c379a75de45236c3813d5b3d9",
        "20a525e3ba881957e42f77ad2db526575bc47af96cd9103a62ebbb645b825e35",
        "7050a8f772e1e4dc33b41f8e103a21b445f113c2c6d7d812715304a5454169cf",
        "c436dd8176e7fc4003e80e3cc5e3b1a8fba60352309d944790427c91768d7c32",
        "394c4f9a4b1d644c602ad03c22438d68108d05bf4bd0c34e453fb8c4604bcb88",
        "56c87f33c75667985a094b9b4ceaca3b734e394296b6ef5146fc432e7d646b69",
        "2edd817e70478b9a168b4fa3aa72f72f3fed7772f09e928622eafe62b162ee83",
    };
    Directory directory;
    int executorCalls = 0;
    for (int index = 0; index < 60; ++index)
    {
        const auto path = directory.root / ("effect-" + std::to_string(index) + ".txt");
        const auto audit = directory.root / ("effect-" + std::to_string(index) + ".jsonl");
        bool failFlush = false;
        EvidenceJournal journal(
            audit, 2, [&](JournalIoBoundary boundary) { return !(failFlush && boundary == JournalIoBoundary::BeforeFlush); });
        const auto transaction = "effect-tx-" + std::to_string(index);
        const auto receipt = journal.Append(Event("effect-intent-" + std::to_string(index), JournalKind::Intent, transaction));
        Check(receipt.Durable(), "Real effect lacked durable intent.");
        ++executorCalls;
        const std::string expected = "mutation-" + std::to_string(index) + '\n';
        {
            std::ofstream output(path, std::ios::binary);
            output << expected;
            Check(output.good(), "Real fixture mutation failed.");
        }
        failFlush = true;
        auto result = Event("effect-result-" + std::to_string(index), JournalKind::Result, transaction);
        result.evidence.digest = expectedDigests[index];
        const auto completion = journal.Append(result);
        Check(!completion.Durable() && completion.state == AppendState::Degraded && journal.Health().degraded,
            "Failed result flush claimed durable completion.");
        const auto originalAudit = Bytes(audit);
        EvidenceJournal restarted(audit);
        const auto health = restarted.Recover();
        Check(health.unresolvedTransactions == std::vector<std::string>{transaction} && health.quarantinedRecords == 1,
            "Restart lost uncertain execution intent or projected uncommitted result.");
        auto query = Query();
        query.transactionId = transaction;
        query.kinds = {JournalKind::Result};
        Check(restarted.Read(query).empty(), "Failed result became a durable result after restart.");
        Check(Bytes(path) == expected && ContentDigest(Bytes(path)) == expectedDigests[index],
            "Independent expected content/digest does not match the actual mutation.");
        Check(Bytes(audit) == originalAudit && executorCalls == index + 1, "Read-only restart modified the journal or replayed an effect.");
        auto verification = Event("reconciled-" + std::to_string(index), JournalKind::Verification, transaction);
        verification.evidence.sourceLocator = path.generic_string();
        verification.evidence.digest = expectedDigests[index];
        Check(restarted.Append(verification).Durable(), "Fresh independent inspection could not reconcile uncertain effect.");
        Check(restarted.Read(query).empty(), "Repair promoted a previously uncommitted result into durable evidence.");
        Check(restarted.Recover().unresolvedTransactions.empty() && Bytes(path) == expected && executorCalls == index + 1,
            "Reconciliation replayed mutation or retained a resolved intent.");
    }
    Check(executorCalls == 60, "Result persistence failure duplicated a real mutation.");
}
void TestCapturedActionScopeProducesExactEvidenceReferences()
{
    Directory directory;
    auto journal = std::make_shared<EvidenceJournal>(directory.root / "scoped-action.jsonl");
    revia::audit::ActionAuditLogger logger(journal);
    auto context = Event("action-context");
    logger.SetScope(context.evidence.scope);
    revia::actions::ActionRequest request;
    request.id = "captured-action";
    request.authorityStamp = context.evidence.stamp;
    request.value = "private typed payload must be omitted";
    revia::actions::PolicyDecision decision;
    Check(logger.RecordIntent(request, decision, "captured-tx"), "Captured action intent failed.");
    Check(
        logger.RecordIntent(request, decision, "captured-tx"), "Captured intent redelivery did not retain its original observation time.");
    const auto references = journal->Read(Query());
    Check(references.size() == 1 && references.front().id == "captured-tx:intent" && references.front().sourceId == "host:action-audit" &&
              references.front().observedAtUnixMs > 0 &&
              references.front().sourceLocator.find("scoped-action.jsonl#captured-tx:intent") != std::string::npos,
        "Action adapter did not publish captured evidence reference.");
    Check(Bytes(journal->Path()).find(request.value) == std::string::npos, "Action evidence leaked typed payload.");
}
void TestDisabledTelemetryQueueStillPersistsDropMarker()
{
    Directory directory;
    EvidenceJournal journal(directory.root / "zero-capacity.jsonl", 0);
    Check(!journal.QueueTelemetry(Event("discarded-telemetry")), "Disabled queue admitted telemetry.");
    Check(journal.FlushTelemetry(), "Disabled queue could not persist its drop marker.");
    const auto records = Decode(Bytes(journal.Path()));
    Check(records.size() == 1 && records[0].at("dropped_count") == 1, "Disabled telemetry queue silently lost its drop count.");
}
void TestKindsLegacyRecordsAndUnsupportedVersions()
{
    Directory directory;
    const auto path = directory.root / "kinds.jsonl";
    const std::string legacy = "{\"action_id\":\"historical\",\"attempted\":true,\"succeeded\":true}\n";
    {
        std::ofstream output(path, std::ios::binary);
        output << legacy;
    }
    EvidenceJournal journal(path);
    const JournalKind kinds[] = {JournalKind::Intent, JournalKind::Observation, JournalKind::Result, JournalKind::Verification,
        JournalKind::Correction, JournalKind::Review};
    const char* names[] = {"intent", "observation", "result", "verification", "correction", "review"};
    for (int index = 0; index < 6; ++index)
        Check(journal.Append(Event("kind-" + std::to_string(index), kinds[index], "kinds-tx")).Durable(), "Known kind failed.");
    Check(Bytes(path).starts_with(legacy), "Historical legacy receipt was changed.");
    const auto records = Decode(Bytes(path));
    Check(records.size() == 7, "Kinds lost a record or rewrote legacy history.");
    for (int index = 0; index < 6; ++index)
    {
        Check(records[index + 1].at("record_type") == names[index], "Journal conflated an evidence kind.");
        auto query = Query();
        query.kinds = {kinds[index]};
        const auto references = journal.Read(query);
        Check(references.size() == 1 && references.front().id == "kind-" + std::to_string(index), "Kind projection changed identity.");
    }
    {
        std::ofstream output(path, std::ios::app);
        output << "{\"journal_version\":2,\"event_id\":\"future\",\"record_type\":\"observation\"}\n";
    }
    const auto original = Bytes(path);
    Check(
        journal.Recover().degraded && journal.Append(Event("blocked-by-version")).state == AppendState::Rejected && Bytes(path) == original,
        "Unknown journal version was accepted or overwritten.");
}
void TestTwentyCrashesDuringRepairCannotCommitAnUnflushedResult()
{
    Directory directory;
    bool failFlush = false;
    EvidenceJournal journal(directory.root / "original.jsonl", 2,
        [&](JournalIoBoundary boundary) { return !(failFlush && boundary == JournalIoBoundary::BeforeFlush); });
    Check(journal.Append(Event("repair-intent", JournalKind::Intent, "repair-tx")).Durable(), "Repair fixture lacks intent.");
    failFlush = true;
    Check(!journal.Append(Event("repair-result", JournalKind::Result, "repair-tx")).Durable(), "Repair fixture did not fail result flush.");
    const auto uncommitted = Bytes(journal.Path());
    failFlush = false;
    Check(journal.Append(Event("post-repair-observation")).Durable(), "Repair fixture append failed.");
    const auto suffix = Bytes(journal.Path()).substr(uncommitted.size());
    for (int crash = 0; crash < 20; ++crash)
    {
        const auto cut = 1 + (suffix.size() - 1) * crash / 19;
        const auto path = directory.root / ("repair-crash-" + std::to_string(crash) + ".jsonl");
        {
            std::ofstream output(path, std::ios::binary);
            output << uncommitted << suffix.substr(0, cut);
        }
        EvidenceJournal restarted(path);
        auto query = Query();
        query.kinds = {JournalKind::Result};
        Check(restarted.Read(query).empty(), "Crash during repair committed an unflushed result.");
        Check(restarted.Recover().unresolvedTransactions == std::vector<std::string>{"repair-tx"},
            "Crash during repair cleared uncertain intent without fresh verification.");
    }
}
void TestOversizedActionResultIsNeverAcknowledgedOrProjected()
{
    Directory directory;
    auto journal = std::make_shared<EvidenceJournal>(directory.root / "oversized.jsonl");
    revia::audit::ActionAuditLogger logger(journal);
    revia::actions::ActionRequest request;
    request.id = "oversized-action";
    revia::actions::PolicyDecision decision;
    Check(logger.RecordIntent(request, decision, "oversized-tx"), "Oversized fixture lacks durable intent.");
    const auto original = Bytes(journal->Path());
    revia::actions::ActionResult result;
    result.attempted = true;
    result.succeeded = true;
    result.message.assign(revia::core::MaximumContractJsonBytes, 'x');
    Check(!logger.Record(request, decision, result, 1.0, "oversized-tx"),
        "Oversized action result was acknowledged then lost from projection.");
    Check(Bytes(journal->Path()) == original && journal->Recover().unresolvedTransactions == std::vector<std::string>{"oversized-tx"},
        "Oversized result changed disk or cleared uncertain intent.");
}
void TestLegacyAdapterUsesJournalAndPreservesReadableFields()
{
    Directory directory;
    auto journal = std::make_shared<EvidenceJournal>(directory.root / "legacy.jsonl");
    revia::audit::ActionAuditLogger logger(journal);
    revia::actions::ActionRequest request;
    request.id = "fixture-action";
    revia::actions::PolicyDecision decision;
    Check(logger.RecordIntent(request, decision, "fixture-transaction"), "Legacy intent failed.");
    Check(logger.RecordIntent(request, decision, "fixture-transaction"), "Legacy duplicate should acknowledge original durable record.");
    revia::actions::ActionResult result;
    result.attempted = true;
    result.succeeded = true;
    Check(logger.Record(request, decision, result, 1.0, "fixture-transaction"), "Legacy result failed.");
    const auto records = Decode(Bytes(journal->Path()));
    Check(records.size() == 2 && records[0].at("journal_version") == 1 && records[0].at("event_id") == "fixture-transaction:intent",
        "Legacy audit does not use versioned stable journal identity.");
    Check(records[0].at("record_type") == "intent" && !records[0].contains("attempted") && !records[0].contains("succeeded") &&
              records[1].at("record_type") == "result" && records[1].at("attempted") == true && records[1].at("succeeded") == true,
        "Legacy audit output lost execution truth or readable fields.");
}
}
void RunEvidenceJournalTests()
{
    TestTelemetryProducerDoesNotWaitForFailedDiskFlush();
    TestDurableDropMarkerIsNotRestoredAfterLaterFailure();
    TestDependentTaskRefusesUnknownEffects();
    TestReplayDeduplicatesAndQuarantinesTwentyCrashTails();
    TestExactScopeAndStampAdmission();
    TestOneHundredDeniedIntentsAndBoundedTelemetry();
    TestSixtyRealEffectsSurviveFailedResultFlushAndReadOnlyRestart();
    TestCapturedActionScopeProducesExactEvidenceReferences();
    TestDisabledTelemetryQueueStillPersistsDropMarker();
    TestKindsLegacyRecordsAndUnsupportedVersions();
    TestTwentyCrashesDuringRepairCannotCommitAnUnflushedResult();
    TestOversizedActionResultIsNeverAcknowledgedOrProjected();
    TestLegacyAdapterUsesJournalAndPreservesReadableFields();
    std::cout << "Evidence journal: 50 records x 20 crash points, 100 denied intents, 100 scope mismatches, "
                 "60 actual effects/failed result flushes/read-only reconciliations, explicit telemetry drops and legacy adapter checks "
                 "passed.\n";
}
#ifdef REVIA_JOURNAL_STANDALONE
int main(int argumentCount, char** arguments)
{
    try
    {
        if (argumentCount > 1 && std::string(arguments[1]) == "repair")
            TestTwentyCrashesDuringRepairCannotCommitAnUnflushedResult();
        else if (argumentCount > 1 && std::string(arguments[1]) == "oversized")
            TestOversizedActionResultIsNeverAcknowledgedOrProjected();
        else if (argumentCount > 1 && std::string(arguments[1]) == "scope")
            TestCapturedActionScopeProducesExactEvidenceReferences();
        else if (argumentCount > 1 && std::string(arguments[1]) == "telemetry")
            TestTelemetryProducerDoesNotWaitForFailedDiskFlush();
        else
            RunEvidenceJournalTests();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
#endif
