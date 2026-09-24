#pragma once

#include "Improvement/codeProposal.h"
#include "Improvement/proposalStore.h"
#include "Improvement/sourceCatalog.h"
#include "Improvement/workbench.h"
#include "Learning/selfAssessment.h"
#include "Library/structLibrary.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace revia::improvement
{

// One review: which code, and why she is looking at it.
struct ReviewJob
{
    // "evidence", "exploration", or "request".
    std::string trigger;
    std::string taskId;
    // The self-assessment category, for an evidence review.
    std::string category;
    // What was measured to be wrong, when something was.
    std::string problem;
    std::string evidence;
    // Metric names recorded where the slow or failing thing happens; the window is
    // centred on the first line that mentions one.
    std::vector<std::string> anchors;
    std::string file;
    std::size_t startLine = 1;
    // What the person typed, for a request.
    std::string request;
};

struct ReviewOutcome
{
    enum class Kind
    {
        // Reviewed; nothing worth changing. The usual result.
        Nothing,
        // She suggested something the checks or thresholds turned away.
        Refused,
        // A proposal was recorded (verified, failed, or awaiting verification).
        Proposed,
        // The review could not run: no model, unreadable file, cancelled.
        Unavailable
    };
    Kind kind = Kind::Nothing;
    std::optional<CodeProposal> proposal;
    std::string note;
    // Where the next exploration of this file should start.
    std::size_t nextLine = 1;
};

// Revia reviewing her own code, end to end: pick what to read, ask for one improvement,
// check it, prove it in the workbench, record it, and say so.
//
// Nothing here can change the real source. The only writer of source files in this
// module is the Workbench, which is confined to its own copy; the proposal itself is a
// patch a person applies.
class ImprovementAgent
{
public:
    // Instructions, the code to review, the reply schema -> the model's reply.
    using ReviewModel = std::function<responseOutput(
        const std::string& instructions, const std::string& material,
        const std::string& schema, std::stop_token stopToken)>;
    using TaskSource = std::function<std::vector<learning::SelfImprovementTask>()>;
    // Whether nobody has used her for this many seconds and she is idle. With
    // `requireSpareResources`, also whether the machine has room for optional work --
    // asked before starting, not while a build runs, or the build's own load would
    // count as a reason to stop it.
    using IdleProbe = std::function<bool(int minimumQuietSeconds, bool requireSpareResources)>;
    // A proven proposal, and the sentence to tell the person.
    using Reporter = std::function<void(const CodeProposal&, const std::string& message)>;
    using LogSink = std::function<void(const std::string&)>;

    struct Dependencies
    {
        SourceCatalog catalog;
        std::shared_ptr<ProposalStore> store;
        // Null when proving is off; proposals are then reported unproven, and say so.
        std::shared_ptr<Workbench> workbench;
        ReviewModel review;
        TaskSource tasks;
        IdleProbe idle;
        Reporter report;
        LogSink log;
    };

    ImprovementAgent() = default;
    ~ImprovementAgent();
    ImprovementAgent(const ImprovementAgent&) = delete;
    ImprovementAgent& operator=(const ImprovementAgent&) = delete;

    // Configures without starting the background loop -- what tests and Run() need.
    void Configure(improvementSettings settings, Dependencies dependencies);
    void Start();
    void Stop();
    [[nodiscard]] bool Running() const;

    // Queues a review of a path, file name, or component. The loop runs it next.
    bool Request(const std::string& target, std::string& outMessage);
    [[nodiscard]] std::string Status() const;

    // One job, start to finish, on the calling thread.
    [[nodiscard]] ReviewOutcome Run(const ReviewJob& job, std::stop_token stopToken);

    // The prompt pieces, exposed so their content can be tested without a model.
    [[nodiscard]] static std::string Instructions(
        const ReviewJob& job, const std::vector<std::string>& lessons);
    [[nodiscard]] static std::string Material(const CodeWindow& window);
    [[nodiscard]] static std::string Schema();

private:
    [[nodiscard]] std::optional<ReviewJob> NextJob();
    void Loop(std::stop_token stopToken);
    // Proves a recorded proposal, with one repair attempt when it does not build.
    void Prove(CodeProposal& proposal, const ReviewJob& job, const std::string& original,
        std::stop_token stopToken);
    void Log(const std::string& line) const;

    mutable std::mutex mutex;
    std::condition_variable_any wake;
    improvementSettings settings;
    Dependencies dependencies;
    std::deque<ReviewJob> requests;
    std::map<std::string, std::chrono::steady_clock::time_point> lastProofAttempt;
    std::string lastActivity = "Not started.";
    std::jthread worker;
};

} // namespace revia::improvement
