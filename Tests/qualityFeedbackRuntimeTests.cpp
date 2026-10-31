#include "reviaSessionTestAccess.h"
#include "testSupport.h"

#include "Audit/contentDigest.h"
#include "Memory/longTermMemory.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace
{
using namespace revia::runtime;
using revia::tests::Check;

revia::learning::QualityFeedback Feedback(ReviaSession& session)
{
    revia::learning::QualityFeedback value;
    value.origin = session.Stamp();
    value.audienceRevision = session.Audience().revision;
    value.judgmentId = "explicit-owner-judgment-1";
    value.sourceId = "owner-review:synthetic-displayed-reply";
    value.targetId = "displayed-reply:synthetic-exact-receipt";
    value.targetDigest = revia::audit::ContentDigest("Actual synthetic displayed reply");
    value.criterion = "Include the explicitly requested public object-file fact.";
    value.evidence = "The owner selected this displayed reply and judged the requested fact absent.";
    value.dependencies = {{"displayed-result", value.targetDigest}, {"answer-contract", "synthetic-v1"}};
    return value;
}

void Prepare(ReviaSession& session, const std::filesystem::path& root)
{
    std::string error;
    Check(ReviaSessionTestAccess::InitializeQualityAssessment(session, root / "assessment.jsonl", error), error);
    ReviaSessionTestAccess::DisableQualityFixtureEmbedding(session);
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);
}

void TestRuntimeJudgmentAuthorityAndAudienceEpoch()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-bridge", "Synthetic quality bridge", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    const auto original = Feedback(session);
    std::string error, taskId, recordId;
    auto invalid = original;
    ++invalid.origin.generation;
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Stale quality origin entered the native bridge.");
    invalid = original;
    invalid.source = revia::learning::QualityEvidenceSource::DeterministicAcceptance;
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Caller forged native acceptance authority.");
    invalid = original;
    invalid.targetDigest = "transport-success";
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Transport success served as an exact target digest.");
    invalid = original;
    invalid.targetId.clear();
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "An absent displayed target entered judged learning.");
    invalid = original;
    invalid.privateContextExcluded = true;
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Excluded private context entered the runtime candidate.");
    invalid = original;
    invalid.evidence = "password=synthetic-secret-credential";
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "A secret entered runtime feedback evidence.");
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "fixture-public", 0, {}}, error), error);
    invalid = Feedback(session);
    Check(!session.RecordQualityFeedback(invalid, taskId, recordId, error), "Public feedback entered private learned history.");
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);
    Check(!session.RecordQualityFeedback(original, taskId, recordId, error),
        "Private-to-public-to-private audience changes reauthorized an old displayed judgment.");
    Check(memory.Load().empty() && session.LearningStudio().lessons.empty(),
        "Refused runtime judgment produced a candidate or trusted memory.");
}

void TestPrivateCandidateCapturedConfigurationAndOrdinaryReview()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-review", "Synthetic private review", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    auto value = Feedback(session);
    value.dependencies["loaded-profile-sha256"] = "forged-profile";
    value.dependencies["effective-llm-config-sha256"] = "forged-configuration";
    std::string error, taskId, recordId;
    Check(session.RecordQualityFeedback(value, taskId, recordId, error), error);
    const auto studio = session.LearningStudio();
    const auto record = std::find_if(studio.lessons.begin(), studio.lessons.end(), [&](const auto& item) { return item.id == recordId; });
    Check(record != studio.lessons.end() && record->disposition == revia::learning::LearningDisposition::PrivateCandidate &&
              memory.Load().empty(),
        "Native owner judgment bypassed ordinary pending learning review.");
    Check(record->candidate.evidence.sources.front() == value.sourceId && record->candidate.evidence.sources.at(1) == value.targetId &&
              record->candidate.lesson.evidence.find(value.targetDigest) != std::string::npos &&
              record->candidate.lesson.evidence.find(value.evidence) != std::string::npos,
        "The runtime changed the owner's exact source, target digest or evidence.");
    const auto& conditions = record->candidate.evidence.conditions;
    Check(std::any_of(conditions.begin(), conditions.end(), [](const auto& text)
              { return text.starts_with("Dependency loaded-profile-sha256: ") && text.find("forged-profile") == std::string::npos; }) &&
              std::any_of(conditions.begin(), conditions.end(),
                  [](const auto& text)
                  {
                      return text.starts_with("Dependency effective-llm-config-sha256: ") &&
                             text.find("forged-configuration") == std::string::npos;
                  }),
        "Caller-controlled dependencies displaced native captured configuration.");
    value.judgmentId = "explicit-owner-judgment-2";
    value.targetId = "displayed-reply:fresh-unchanged-configuration";
    value.targetDigest = revia::audit::ContentDigest("Fresh result under unchanged native configuration");
    value.dependencies["displayed-result"] = value.targetDigest;
    Check(!session.RetestQualityFeedback(taskId, value, error), "New displayed output counted as changed native configuration.");
    ReviaSessionTestAccess::PlannedMainDevice(session, "synthetic-new-device");
    Check(session.RetestQualityFeedback(taskId, value, error), error);
    value.judgmentId = "explicit-owner-relabel-same-failed-result";
    value.evidence = "The same failed retest was relabeled satisfactory without a new result.";
    value.criterionSatisfied = true;
    Check(!session.RetestQualityFeedback(taskId, value, error, true),
        "Runtime owner judgment resolved the task using an already-failed retest result.");
    value.judgmentId = "explicit-owner-judgment-3";
    value.targetId = "displayed-reply:fresh-held-out-acceptance";
    value.targetDigest = revia::audit::ContentDigest("Fresh held-out reply contains the requested public fact");
    value.evidence = "The owner checked the fresh held-out reply and found the requested fact present.";
    value.criterionSatisfied = true;
    Check(session.RetestQualityFeedback(taskId, value, error, true), error);
    Check(memory.Load().empty(), "Criterion satisfaction automatically inserted trusted memory.");
    Check(session.ReviewLearning(
              recordId, revia::learning::LearningDecision::Accept, "The owner accepts this exact scoped quality observation.", error),
        error);
    Check(memory.Load().size() == 1 && memory.Load().front().source == "reviewed_lesson:" + recordId &&
              memory.Load().front().category == "constraint",
        "Explicit ordinary learning review did not produce the actual quality-memory receipt.");
}

responseOutput HeldReply(const std::string& displayed)
{
    responseOutput reply;
    reply.bSuccess = true;
    reply.response = reply.rawResponse = displayed;
    auto exchange = std::make_shared<revia::llm::ProviderExchange>();
    exchange->requestBody = nlohmann::json{{"model", "main"},
        {"messages", {{{"role", "system"}, {"content", "You are a synthetic companion."}},
                         {{"role", "user"}, {"content", "Which object file does the linker need?"}}}},
        {"chat_template_kwargs", {{"enable_thinking", false}}}, {"stream", true}}
                                .dump();
    exchange->responseText = displayed;
    exchange->finishReason = "stop";
    exchange->completeStream = true;
    exchange->model = "main";
    exchange->templateSha256 = revia::audit::ContentDigest("{{ messages }}");
    reply.exchange = std::move(exchange);
    return reply;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void TestJudgedReplyExchangeIsKeptOnlyOnRequestAndWithdrawn()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"capture-bridge", "Synthetic capture bridge", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    const auto feedback = Feedback(session);
    std::string captureId, error;
    Check(!session.CaptureJudgedTurn(feedback, captureId, error) && error.find("no longer held") != std::string::npos,
        "A reply that was never held was kept.");
    ReviaSessionTestAccess::HoldDeliveredReply(session, HeldReply("Actual synthetic displayed reply"), "owner");
    Check(!std::filesystem::exists(paths.Resolve("RuntimeData/Learning/Captures")), "Holding a delivered reply wrote it to disk.");
    auto invalid = feedback;
    invalid.criterionSatisfied = true;
    Check(!session.CaptureJudgedTurn(invalid, captureId, error), "A satisfied criterion kept an exchange.");
    invalid = feedback;
    ++invalid.origin.generation;
    Check(!session.CaptureJudgedTurn(invalid, captureId, error), "A stale origin kept an exchange.");

    std::string taskId, recordId;
    Check(session.RecordQualityFeedback(feedback, taskId, recordId, error), error);
    Check(session.CaptureJudgedTurn(feedback, captureId, error), "The judged, held reply was not kept: " + error);
    const auto kept = paths.Resolve("RuntimeData/Learning/Captures/live") / (captureId + ".json");
    const auto record = nlohmann::json::parse(ReadFile(kept));
    const auto sources = record["sources"].get<std::vector<std::string>>();
    Check(record["judgment"]["judgmentId"] == feedback.judgmentId && record["judgment"]["targetDigest"] == feedback.targetDigest &&
              record["judgment"]["dependencies"].contains("loaded-profile-sha256") && record["origin"]["sessionId"] == session.Stamp().sessionId &&
              record["source"]["participantId"] == "owner" &&
              std::none_of(sources.begin(), sources.end(), [](const std::string& source) { return source.starts_with("participant:"); }),
        "The kept capture lost the owner's judgment, the captured fingerprints, its origin or its speaker.");
    std::string again;
    Check(!session.CaptureJudgedTurn(feedback, again, error) && error.find("already kept") != std::string::npos,
        "The same judged reply was kept twice.");

    // A configuration change after the reply means the exchange is no longer what the
    // judgment's fingerprints describe.
    ReviaSessionTestAccess::HoldDeliveredReply(session, HeldReply("Second synthetic displayed reply"), "owner");
    ReviaSessionTestAccess::PlannedMainDevice(session, "synthetic-new-device");
    auto later = feedback;
    later.judgmentId = "explicit-owner-judgment-later";
    later.targetId = "displayed-reply:second";
    later.targetDigest = revia::audit::ContentDigest("Second synthetic displayed reply");
    later.dependencies["displayed-result"] = later.targetDigest;
    Check(!session.CaptureJudgedTurn(later, again, error) && error.find("changed after this reply") != std::string::npos,
        "An exchange was kept under a configuration that did not produce it.");
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "fixture-public", 0, {}}, error), error);
    Check(!session.CaptureJudgedTurn(Feedback(session), again, error) && error.find("current private session") != std::string::npos,
        "A public audience kept a private exchange.");
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);
    // Back in private, the replies held before the audience changed belong to an older epoch.
    Check(!session.CaptureJudgedTurn(Feedback(session), again, error) && error.find("no longer held") != std::string::npos,
        "A reply held under an earlier audience was kept under a new one.");

    // Held under the current audience, so only forgetting the chats can drop it.
    ReviaSessionTestAccess::HoldDeliveredReply(session, HeldReply("Actual synthetic displayed reply"), "owner");
    session.ForgetConversations();
    const auto withdrawn = nlohmann::json::parse(ReadFile(paths.Resolve("RuntimeData/Learning/Captures/withdrawn.json")))
                               .get<std::vector<std::string>>();
    Check(!std::filesystem::exists(kept) && std::find(withdrawn.begin(), withdrawn.end(), feedback.targetId) != withdrawn.end(),
        "Forgetting every chat left its kept capture, or did not list its judged reply as withdrawn.");
    Check(!session.CaptureJudgedTurn(Feedback(session), again, error) && error.find("no longer held") != std::string::npos,
        "A forgotten chat's reply was still held in memory.");
}

void TestOwnerCorrectsKeptExchangesOnlyInPrivate()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"correction-bridge", "Synthetic correction bridge", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    const auto feedback = Feedback(session);
    std::string captureId, taskId, recordId, error;
    Check(session.LearningStudio().captures.empty(), "Captures were listed before any was kept.");
    ReviaSessionTestAccess::HoldDeliveredReply(session, HeldReply("Actual synthetic displayed reply"), "owner");
    Check(session.RecordQualityFeedback(feedback, taskId, recordId, error) && session.CaptureJudgedTurn(feedback, captureId, error), error);
    auto studio = session.LearningStudio();
    Check(studio.captures.size() == 1 && studio.captures[0].captureId == captureId &&
              studio.captures[0].decision == revia::learning::LearningDecision::Pending &&
              studio.captures[0].input == "Which object file does the linker need?" && studio.captures[0].criterion == feedback.criterion,
        "The kept exchange was not listed for correction with its judgment.");

    std::uint64_t revision = 0;
    const std::string correct = "The linker needs the object file the build produced for that unit.";
    Check(session.CorrectLiveCapture(captureId, revia::learning::LearningDecision::Accept, correct, revision, error) && revision == 1,
        "The owner's correction was not saved: " + error);
    studio = session.LearningStudio();
    Check(studio.captures.size() == 1 && studio.captures[0].decision == revia::learning::LearningDecision::Accept &&
              studio.captures[0].correction == correct && studio.captures[0].revision == 1,
        "A saved correction was not shown on the next refresh.");

    // This one example falls in the test partition of this companion's lineage, so there is
    // nothing to train on, and nothing is written.
    const auto exports = fixture.root / "exports";
    std::filesystem::create_directories(exports);
    revia::learning::LiveDatasetExport exported;
    Check(!session.ExportLiveDataset(exports, exported, error) && error.find("held-out partition") != std::string::npos &&
              std::filesystem::is_empty(exports) && !std::filesystem::exists(paths.Resolve("RuntimeData/Learning/Datasets/lineage.json")),
        "An export with nothing to train on was written: " + error);

    Check(session.SetAudience({revia::identity::AudienceKind::Public, "fixture-public", 0, {}}, error), error);
    studio = session.LearningStudio();
    Check(studio.captures.empty() && studio.captureStatus.find("not private") != std::string::npos,
        "Kept exchanges were listed for a public audience.");
    Check(!session.ExportLiveDataset(exports, exported, error) && error.find("private") != std::string::npos,
        "A public audience exported private training data.");
    Check(!session.CorrectLiveCapture(captureId, revia::learning::LearningDecision::Reject, "", revision, error) &&
              !session.DiscardLiveCapture(captureId, error),
        "A public audience changed a kept exchange.");
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);

    const auto captures = paths.Resolve("RuntimeData/Learning/Captures");
    Check(session.DiscardLiveCapture(captureId, error), "The owner could not delete a kept exchange: " + error);
    Check(session.LearningStudio().captures.empty() && !std::filesystem::exists(captures / "live" / (captureId + ".json")) &&
              !std::filesystem::exists(captures / "corrections" / (captureId + ".json")),
        "Deleting a kept exchange left it, its correction or its listing behind.");
    const auto withdrawn = nlohmann::json::parse(ReadFile(captures / "withdrawn.json")).get<std::vector<std::string>>();
    Check(withdrawn == std::vector<std::string>{feedback.targetId}, "The deleted exchange was not listed as withdrawn by its judged reply.");
}

void TestOwnerExportsCorrectionsAsNumberedDatasets()
{
    revia::tests::ScopedTestDirectory fixture;
    // In this companion's lineage the one example below falls in the training partition.
    const CompanionPaths paths(fixture.root, {"dataset-bridge", "Synthetic dataset bridge", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    const auto feedback = Feedback(session);
    std::string captureId, taskId, recordId, error;
    ReviaSessionTestAccess::HoldDeliveredReply(session, HeldReply("Actual synthetic displayed reply"), "owner");
    Check(session.RecordQualityFeedback(feedback, taskId, recordId, error) && session.CaptureJudgedTurn(feedback, captureId, error), error);
    std::uint64_t revision = 0;
    Check(session.CorrectLiveCapture(captureId, revia::learning::LearningDecision::Accept,
              "The linker needs the object file the build produced for that unit.", revision, error),
        error);
    const auto exports = fixture.root / "exports";
    std::filesystem::create_directories(exports);
    revia::learning::LiveDatasetExport exported;
    Check(session.ExportLiveDataset(exports, exported, error), "The owner's corrections were not exported: " + error);
    Check(exported.name == "dataset-bridge-live-1" && exported.train == 1 && std::filesystem::exists(exported.directory / "train.jsonl") &&
              std::filesystem::exists(paths.Resolve("RuntimeData/Learning/Datasets/lineage.json")) &&
              std::filesystem::exists(exported.withdrawnList),
        "The export is not the companion's first numbered dataset with its lineage and withdrawal list.");
    revia::learning::LiveDatasetExport second;
    Check(session.ExportLiveDataset(exports, second, error) && second.name == "dataset-bridge-live-2",
        "A second export did not take the next number: " + error);
}

void TestRuntimeMemorySelectionAndAudienceEpoch()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"revision-bridge", "Synthetic revision bridge", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    memoryDecision original;
    original.bSuccess = original.bShouldRemember = true;
    original.category = "project";
    original.summary = "The owner tracks Amber at eight.";
    bool added;
    std::string originalId, error;
    Check(memory.Save(original, added, &originalId), "Runtime revision original fixture save failed.");
    revia::memory::MemoryRevisionRequest request;
    request.ownerRequestId = "owner-selected-revision-1";
    request.originalId = originalId;
    request.expectedSummaryDigest = revia::audit::ContentDigest(original.summary);
    request.origin = session.Stamp();
    request.audienceRevision = session.Audience().revision;
    request.corrected = original;
    request.corrected.summary = "The owner tracks Amber at nine.";
    request.reason = "The owner selected this exact retained record and requested replacement.";
    request.evidence = "Explicit selected-row replacement supplied by the owner.";
    revia::memory::MemoryRevisionReceipt receipt;
    auto invalid = request;
    ++invalid.origin.generation;
    Check(!session.ReviseMemoryOwnerRequested(invalid, receipt, error), "Stale origin revised native memory.");
    invalid = request;
    invalid.expectedSummaryDigest = revia::audit::ContentDigest("Unselected different text");
    Check(!session.ReviseMemoryOwnerRequested(invalid, receipt, error), "Unselected digest revised native memory.");
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "fixture-public", 0, {}}, error), error);
    invalid = request;
    invalid.audienceRevision = session.Audience().revision;
    Check(!session.ReviseMemoryOwnerRequested(invalid, receipt, error), "Public context revised private memory.");
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "fixture-private", 0, {}}, error), error);
    Check(!session.ReviseMemoryOwnerRequested(request, receipt, error), "A new audience epoch reauthorized old memory selection.");
    Check(memory.Load().size() == 1 && memory.RevisionHistory().empty(), "Refused bridge revision changed storage.");
    request.audienceRevision = session.Audience().revision;
    Check(session.ReviseMemoryOwnerRequested(request, receipt, error), error);
    Check(receipt.originalId == originalId && receipt.audienceRevision == request.audienceRevision && memory.Load().size() == 2 &&
              memory.RevisionHistory().size() == 1,
        "Exact private owner selection lost its atomic revision or audience receipt.");
}

void TestCanceledQualityMutationsDoNotWriteAfterForegroundWait()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-cancellation", "Synthetic cancellation", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    longTermMemory memory(paths.Resolve("Memory/revia_memory.db").string());
    memoryDecision original;
    original.bSuccess = original.bShouldRemember = true;
    original.category = "project";
    original.summary = "The owner tracks Amber at eight.";
    bool added;
    std::string originalId, error, taskId, recordId;
    Check(memory.Save(original, added, &originalId), "Cancellation fixture original save failed.");
    const auto feedback = Feedback(session);
    revia::memory::MemoryRevisionRequest revision;
    revision.ownerRequestId = "owner-canceled-revision";
    revision.originalId = originalId;
    revision.expectedSummaryDigest = revia::audit::ContentDigest(original.summary);
    revision.origin = session.Stamp();
    revision.audienceRevision = session.Audience().revision;
    revision.corrected = original;
    revision.corrected.summary = "The owner tracks Amber at nine.";
    revision.reason = "Explicit exact owner replacement.";
    revision.evidence = "Selected original and supplied replacement.";
    revia::memory::MemoryRevisionReceipt receipt;
    std::stop_source canceled;
    canceled.request_stop();
    Check(!session.RecordQualityFeedback(feedback, taskId, recordId, error, canceled.get_token()),
        "Already-canceled owner review wrote judged quality state.");
    Check(!session.ReviseMemoryOwnerRequested(revision, receipt, error, canceled.get_token()),
        "Already-canceled owner revision changed memory.");
    for (const bool reviseMemory : {false, true})
    {
        std::stop_source stop;
        auto foreground = ReviaSessionTestAccess::HoldForeground(session);
        std::promise<void> launched;
        auto entered = launched.get_future();
        auto mutation = std::async(std::launch::async,
            [&]
            {
                launched.set_value();
                if (reviseMemory)
                {
                    return session.ReviseMemoryOwnerRequested(revision, receipt, error, stop.get_token());
                }
                return session.RecordQualityFeedback(feedback, taskId, recordId, error, stop.get_token());
            });
        const bool held = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                          mutation.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
        stop.request_stop();
        foreground.unlock();
        const bool changed = mutation.get();
        Check(held, "Cancellation fixture did not hold the actual foreground mutation lock.");
        Check(!changed, "Canceled mutation resumed and wrote after foreground lock release.");
    }
    Check(taskId.empty() && recordId.empty() && receipt.requestId.empty() && !std::filesystem::exists(fixture.root / "assessment.jsonl") &&
              session.LearningStudio().lessons.empty() && memory.Load().size() == 1 && memory.RevisionHistory().empty(),
        "Canceled review/revision left a task, candidate, corrected memory or receipt.");
}

void TestCanceledQualityRetestPreservesAttemptHistory()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"quality-retest-cancellation", "Synthetic retest cancellation", "assistant", false});
    ReviaSession session(paths);
    Prepare(session, fixture.root);
    auto feedback = Feedback(session);
    std::string error, taskId, recordId;
    Check(session.RecordQualityFeedback(feedback, taskId, recordId, error), error);
    feedback.judgmentId = "canceled-retest-judgment";
    feedback.targetId = "displayed-reply:canceled-fresh-held-out-result";
    feedback.targetDigest = revia::audit::ContentDigest("Fresh held-out result judged satisfactory");
    feedback.evidence = "The owner checked the fresh held-out result against the recorded criterion.";
    feedback.criterionSatisfied = true;
    std::stop_source canceled;
    canceled.request_stop();
    Check(!session.RetestQualityFeedback(taskId, feedback, error, true, canceled.get_token()),
        "Already-canceled held-out judgment resolved a quality gap.");
    std::stop_source stop;
    auto foreground = ReviaSessionTestAccess::HoldForeground(session);
    std::promise<void> launched;
    auto entered = launched.get_future();
    auto retest = std::async(std::launch::async,
        [&]
        {
            launched.set_value();
            return session.RetestQualityFeedback(taskId, feedback, error, true, stop.get_token());
        });
    const bool held = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                      retest.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
    stop.request_stop();
    foreground.unlock();
    const bool changed = retest.get();
    Check(held && !changed, "Canceled held-out judgment resolved a gap after foreground lock release.");
    revia::learning::SelfAssessmentEngine recovered;
    Check(recovered.Initialize(fixture.root / "assessment.jsonl", error), error);
    Check(recovered.Snapshot().openTasks.size() == 1 && recovered.Snapshot().openTasks.front().gapAttempts.size() == 1,
        "Canceled retest wrote an attempt or resolution to the durable quality history.");
}
}

void RunQualityFeedbackRuntimeTests()
{
    TestRuntimeJudgmentAuthorityAndAudienceEpoch();
    TestPrivateCandidateCapturedConfigurationAndOrdinaryReview();
    TestJudgedReplyExchangeIsKeptOnlyOnRequestAndWithdrawn();
    TestOwnerCorrectsKeptExchangesOnlyInPrivate();
    TestOwnerExportsCorrectionsAsNumberedDatasets();
    TestRuntimeMemorySelectionAndAudienceEpoch();
    TestCanceledQualityMutationsDoNotWriteAfterForegroundWait();
    TestCanceledQualityRetestPreservesAttemptHistory();
    std::cout << "Runtime quality feedback checks passed: owner authority, audience epochs, captured dependencies, ordinary reviewed "
                 "receipts, judged exchanges kept only on request and withdrawn with their chats, private owner corrections and numbered dataset exports.\n";
}
