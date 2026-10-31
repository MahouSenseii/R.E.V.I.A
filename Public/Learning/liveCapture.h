#pragma once

#include "LLM/providerExchange.h"
#include "Learning/qualityFeedback.h"
#include "Runtime/runtimeStamp.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace revia::learning
{

// A reply the companion delivered in a private conversation, held in memory with the exact
// exchange that produced it, so an owner judgment of the displayed answer can be bound to
// that request. Nothing here is written anywhere until the owner asks to keep a judged reply.
struct DeliveredReply
{
    runtime::RuntimeStamp origin;
    std::uint64_t audienceRevision = 0;
    // The archived conversation and the attributed speaker the turn belonged to.
    std::string conversationId;
    std::string participantId;
    // The text the owner saw. A judgment's target digest must be this text's digest.
    std::string displayed;
    // The model's visible text before style repair.
    std::string modelText;
    // A safety replacement or another model's rewrite: the displayed text is not this
    // request's answer at all.
    bool replaced = false;
    // The loaded-profile-sha256 and effective-llm-config-sha256 fingerprints at delivery.
    std::string profileDigest;
    std::string configurationDigest;
    std::shared_ptr<const llm::ProviderExchange> exchange;
};

struct LiveCapture
{
    std::string captureId;
    // The exact bytes of the capture file.
    std::string record;
    // Withdrawing any of these withdraws the capture.
    std::vector<std::string> sources;
};

inline constexpr const char* LiveCaptureVersion = "live-capture-v1";

// One kept capture as the owner reviews it, with the decision and correction saved so far.
struct LiveCaptureSummary
{
    std::string captureId;
    // What the person said, without the runtime context the prompt placed ahead of it.
    std::string input;
    // The judged reply, and the model's own text when style repair changed it.
    std::string failedOutput;
    std::string failedOutputSha256;
    std::string modelOutput;
    bool repaired = false;
    // The judgment the owner recorded when the capture was kept.
    std::string issue;
    std::string criterion;
    std::string evidence;
    std::string conversationId;
    // Pending until the owner saves a decision; revision 0 means none was saved.
    LearningDecision decision = LearningDecision::Pending;
    std::string correction;
    std::uint64_t revision = 0;
};

// The capture of one judged reply in the interchange revia_dataset_capture.py reads, or why
// it cannot be kept exactly. Refuses anything a text dataset cannot reproduce or the runtime
// cannot later withdraw: pictures, tool steps, thinking, private reference material without
// a named source, and anything sensitive.
bool BuildLiveCapture(const DeliveredReply& reply, const QualityFeedback& judgment, LiveCapture& outCapture, std::string& outError);

// The companion's kept captures, one file each under live/, and withdrawn.json beside them:
// every source withdrawn since, in the list ReviaDatasetExport --withdrawn and a training job
// read, so a copy already exported elsewhere is refused too.
class LiveCaptureStore
{
public:
    explicit LiveCaptureStore(std::filesystem::path root);

    // Writes a new capture and never replaces one.
    bool Keep(const LiveCapture& capture, std::string& outError) const;
    // Deletes every kept capture that names `source`, with its correction, then records `source`
    // and each deleted capture's judged reply (displayed-reply:...) as withdrawn. A
    // participant:<id> source matches the speaker a capture recorded. Nothing happens before
    // the first capture is kept.
    bool Withdraw(const std::string& source, std::size_t& outRemoved, std::string& outError) const;
    // The same for every kept capture, recording each one's conversation and judged reply.
    bool WithdrawAll(std::size_t& outRemoved, std::string& outError) const;

    // Every kept capture with the owner's latest decision, newest first. A file that cannot be
    // read is left out, and outError says how many were.
    [[nodiscard]] std::vector<LiveCaptureSummary> List(std::string& outError) const;
    // Saves the owner's decision and correct reply for one capture as its next revision,
    // keeping the earlier ones. An accepted capture needs a correction that differs from the
    // judged reply. Pending is not a decision to save.
    bool Correct(const std::string& captureId, LearningDecision decision, const std::string& correction, std::uint64_t& outRevision,
        std::string& outError) const;
    // Deletes one capture with its correction, and lists its judged reply as withdrawn. Its
    // request is not listed: a regenerated reply judged separately can share it.
    bool Discard(const std::string& captureId, std::string& outError) const;
    // Changes when a capture or correction is added, replaced or removed, so a caller can
    // keep List's result until then.
    [[nodiscard]] std::string Revision() const;
    // The decided captures as the candidate interchange (revia-dataset-candidates-v1), exactly
    // as revia_dataset_capture.py candidates --prefix live writes it from the saved
    // corrections. Captures without a decision are counted in outWaiting. Any unreadable or
    // inconsistent capture or correction refuses the whole file.
    bool CandidateFile(std::string& outJson, std::size_t& outCandidates, std::size_t& outWaiting, std::string& outError) const;

    [[nodiscard]] std::filesystem::path LiveDirectory() const;
    [[nodiscard]] std::filesystem::path CorrectionsDirectory() const;
    [[nodiscard]] std::filesystem::path WithdrawnList() const;

private:
    // Deletes the captures `source` names, only `onlyCapture` when it is set, or all of them.
    bool WithdrawMatching(const std::string* source, const std::string* onlyCapture, std::size_t& outRemoved, std::string& outError) const;
    std::filesystem::path root;
};

} // namespace revia::learning
