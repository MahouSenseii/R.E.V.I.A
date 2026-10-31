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
    // Deletes every kept capture that names `source`, then records `source` and each deleted
    // capture's request as withdrawn. Nothing happens before the first capture is kept.
    bool Withdraw(const std::string& source, std::size_t& outRemoved, std::string& outError) const;
    // The same for every kept capture, recording each one's conversation and request.
    bool WithdrawAll(std::size_t& outRemoved, std::string& outError) const;
    [[nodiscard]] std::filesystem::path LiveDirectory() const;
    [[nodiscard]] std::filesystem::path WithdrawnList() const;

private:
    bool WithdrawMatching(const std::string* source, std::size_t& outRemoved, std::string& outError) const;
    std::filesystem::path root;
};

} // namespace revia::learning
