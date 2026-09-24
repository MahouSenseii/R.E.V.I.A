#include "Improvement/improvementAgent.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace revia::improvement
{

namespace
{

// Two minutes of quiet before she spends the GPU on reading her own code. A turn still
// preempts the review, which runs at background priority.
constexpr int ReviewQuietSeconds = 120;
constexpr std::size_t MaximumWindowCharacters = 12000;

std::int64_t NowEpoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// One evidence review per problem kind per week. Older history holds several open tasks
// for the same symptom; each would otherwise send her back to the same code.
std::string CategoryWeekKey(const std::string& category)
{
    return "category:" + category + "@" + std::to_string(NowEpoch() / (7 * 24 * 3600));
}

std::string Short(std::string text, const std::size_t limit)
{
    std::replace(text.begin(), text.end(), '\n', ' ');
    if (text.size() > limit) text = utf8::Prefix(text, limit) + "...";
    return text;
}

// "Private/Speech/speechService.cpp:700" -> the path and line 700.
std::pair<std::string, std::size_t> SplitLineSuffix(const std::string& target)
{
    const std::size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon + 1 >= target.size() || colon == 1) return {target, 1};
    const std::string digits = target.substr(colon + 1);
    if (!std::all_of(digits.begin(), digits.end(),
            [](const unsigned char character) { return std::isdigit(character) != 0; }) ||
        digits.size() > 7)
    {
        return {target, 1};
    }
    return {target.substr(0, colon), static_cast<std::size_t>(std::stoul(digits))};
}

} // namespace

ImprovementAgent::~ImprovementAgent()
{
    Stop();
}

void ImprovementAgent::Configure(improvementSettings inputSettings, Dependencies inputDependencies)
{
    std::lock_guard lock(mutex);
    settings = std::move(inputSettings);
    settings.windowLines = std::clamp(settings.windowLines, 60, 400);
    settings.minimumBenefit = std::clamp(settings.minimumBenefit, 0.0, 1.0);
    settings.evidenceMinimumBenefit = std::clamp(settings.evidenceMinimumBenefit, 0.0, 1.0);
    settings.maximumRisk = std::clamp(settings.maximumRisk, 0.0, 1.0);
    dependencies = std::move(inputDependencies);
}

void ImprovementAgent::Start()
{
    if (worker.joinable()) return;
    {
        std::lock_guard lock(mutex);
        lastActivity = "Waiting for something worth reviewing.";
    }
    worker = std::jthread([this](const std::stop_token stopToken) { Loop(stopToken); });
}

void ImprovementAgent::Stop()
{
    if (!worker.joinable()) return;
    worker.request_stop();
    wake.notify_all();
    worker.join();
}

bool ImprovementAgent::Running() const
{
    return worker.joinable();
}

void ImprovementAgent::Log(const std::string& line) const
{
    if (dependencies.log) dependencies.log("[Improvement] " + line);
}

bool ImprovementAgent::Request(const std::string& target, std::string& outMessage)
{
    const auto [wanted, line] = SplitLineSuffix(target);
    std::vector<std::string> files;
    {
        std::lock_guard lock(mutex);
        if (!dependencies.catalog.Valid())
        {
            outMessage = "Self-review is off: there is no source tree beside this build.";
            return false;
        }
        files = dependencies.catalog.Resolve(wanted);
    }
    if (files.empty())
    {
        outMessage = "I couldn't find any of my code matching \"" + wanted + "\". Name a file "
            "under Private/, Public/ or Desktop/, or a component like voice or memory.";
        return false;
    }
    ReviewJob job;
    job.trigger = "request";
    job.file = files.front();
    job.startLine = line;
    job.request = target;
    {
        std::lock_guard lock(mutex);
        requests.push_back(job);
    }
    wake.notify_all();
    outMessage = "I'll review " + job.file + (line > 1 ? " from line " + std::to_string(line) : "") +
        " next and tell you if I find something worth changing." +
        (files.size() > 1 ? " (Also related: " + files[1] + ".)" : "");
    return true;
}

std::string ImprovementAgent::Status() const
{
    std::lock_guard lock(mutex);
    std::ostringstream text;
    if (!settings.bEnabled || !dependencies.catalog.Valid())
    {
        text << "Self-review is off" << (settings.bEnabled ? ": no source tree beside this build."
            : " in settings.") << "\n";
        return text.str();
    }
    std::size_t counts[5] = {};
    const std::vector<CodeProposal> all =
        dependencies.store ? dependencies.store->All() : std::vector<CodeProposal>{};
    for (const CodeProposal& proposal : all) ++counts[static_cast<int>(proposal.status)];
    text << "Self-review of " << dependencies.catalog.Files().size() << " source files in "
         << dependencies.catalog.Root().generic_string() << "\n"
         << "Proposals: " << all.size() << " (" << counts[1] << " proven and waiting for you, "
         << counts[0] << " not yet proven, " << counts[2] << " failed, " << counts[3]
         << " accepted, " << counts[4] << " rejected)\n"
         << "Proving: " << (settings.bVerify && dependencies.workbench
                ? "build + all test suites in " + dependencies.workbench->Root().generic_string()
                : std::string("off")) << "\n"
         << "Reviewing on her own: " << (settings.bExplore ? "yes, while you're away" : "no")
         << "\n"
         << "Last: " << lastActivity << "\n";
    return text.str();
}

std::string ImprovementAgent::Schema()
{
    return R"({"type":"object","properties":{)"
        // The prose is bounded so the token budget is left for the code, which is not:
        // a cap on find would only turn an edit that is too long into one that is wrong.
        R"("found":{"type":"boolean"},)"
        R"("title":{"type":"string","maxLength":120},)"
        R"("problem":{"type":"string","maxLength":600},)"
        R"("reason":{"type":"string","maxLength":600},)"
        R"("evidence":{"type":"string","maxLength":700},)"
        R"("expected_benefit":{"type":"string","maxLength":300},)"
        R"("risks":{"type":"string","maxLength":300},)"
        R"("benefit":{"type":"number"},)"
        R"("risk":{"type":"number"},)"
        R"("file":{"type":"string","maxLength":200},)"
        R"("find":{"type":"string"},)"
        R"("replace":{"type":"string"}},)"
        // Every field, even when nothing was found: with only "found" and "reason"
        // required, the 8B model claimed findings and left out the code to change.
        R"("required":["found","title","problem","reason","evidence","expected_benefit",)"
        R"("risks","benefit","risk","file","find","replace"]})";
}

std::string ImprovementAgent::Instructions(
    const ReviewJob& job, const std::vector<std::string>& lessons)
{
    std::ostringstream text;
    text << "You are Revia, reviewing your own source code: C++20 on Windows, and the Python "
            "voice worker. You are looking for ONE real improvement in the code shown.\n\n";
    if (job.trigger == "evidence")
    {
        text << "Why you are looking: your self-assessment measured a real problem in you.\n"
             << "Problem: " << job.problem << "\n"
             << "Evidence: " << job.evidence << "\n"
             << "Look in this code for the cause, or for a change that measurably helps.\n\n";
    }
    else if (job.trigger == "request")
    {
        text << "Why you are looking: the person you work with asked you to review this code (\""
             << job.request << "\").\n\n";
    }
    else
    {
        text << "Why you are looking: routine self-review. Nothing is known to be wrong here, "
                "so only report a change that is clearly worth making.\n\n";
    }
    text << "What counts as an improvement: a bug (wrong result, crash, data race, deadlock, "
            "leak, unbounded growth), needless work in a hot path, a missing check that causes "
            "a real failure, or a fix for the measured problem above.\n"
            "What does not count: renames, formatting, comments, style, 'modernising', "
            "speculative refactors, extra logging, or anything you cannot justify from the "
            "code shown.\n\n"
            "Rules:\n"
            "- One change, in the file shown, as small as it can be (at most about 40 lines).\n"
            "- \"find\" is copied character for character from the code shown: whole lines, "
            "nothing shortened, never \"...\" in place of anything, and enough lines to be "
            "unique in the file. \"replace\" is the complete new text for those lines.\n"
            "- Match the surrounding style: 4-space indentation, the same naming.\n"
            "- Never add code that starts processes, uses the network, deletes files, or "
            "touches the registry.\n"
            "- The comments here record real past bugs and deliberate decisions. Do not undo "
            "something a comment explains unless you can show the comment is wrong.\n"
            "- If you are not sure the change is correct AND worth it, return found=false with "
            "the reason. Finding nothing is the normal, correct result of most reviews.\n\n"
            "Scores: benefit 0 to 1 (0.3 minor, 0.6 a clear real improvement, 0.9 fixes a bug "
            "someone would hit). risk 0 to 1 (the chance the change breaks something).\n"
            "Write problem, reason and evidence for the person who will read them: what is wrong, "
            "why your change fixes it, and which lines show it.\n";
    if (!lessons.empty())
    {
        text << "\nWhat happened to your earlier proposals. Learn from these; do not repeat a "
                "rejected idea:\n";
        for (const std::string& lesson : lessons) text << "- " << lesson << "\n";
    }
    text << "\nReply with the JSON object only.";
    return text.str();
}

std::string ImprovementAgent::Material(const CodeWindow& window)
{
    std::ostringstream text;
    text << "File: " << window.path << " (lines " << window.firstLine << "-" << window.lastLine
         << " of " << window.totalLines << ")\n\n" << window.text;
    return text.str();
}

ReviewOutcome ImprovementAgent::Run(const ReviewJob& job, const std::stop_token stopToken)
{
    ReviewOutcome outcome;
    improvementSettings current;
    Dependencies use;
    {
        std::lock_guard lock(mutex);
        current = settings;
        use = dependencies;
    }
    const auto remember = [this](const std::string& line)
    {
        std::lock_guard lock(mutex);
        lastActivity = line;
    };
    if (!use.review || !use.store || !use.catalog.Valid())
    {
        outcome.kind = ReviewOutcome::Kind::Unavailable;
        outcome.note = "Self-review is not set up.";
        return outcome;
    }

    // A proposal waiting only for its proof.
    if (job.trigger == "prove")
    {
        std::optional<CodeProposal> proposal = use.store->Find(job.taskId);
        std::string original;
        std::string error;
        if (!proposal || !use.catalog.Read(proposal->change.path, original, error))
        {
            outcome.kind = ReviewOutcome::Kind::Unavailable;
            outcome.note = proposal ? error : "The proposal to prove is gone.";
            return outcome;
        }
        {
            std::lock_guard lock(mutex);
            lastProofAttempt[proposal->id] = std::chrono::steady_clock::now();
        }
        ReviewJob proofJob = job;
        proofJob.trigger = proposal->trigger;
        proofJob.file = proposal->change.path;
        Prove(*proposal, proofJob, original, stopToken);
        outcome.kind = ReviewOutcome::Kind::Proposed;
        outcome.proposal = proposal;
        remember("Proof of #" + proposal->id + ": " + proposal->verificationSummary);
        return outcome;
    }

    std::string content;
    std::string error;
    if (!use.catalog.Read(job.file, content, error))
    {
        outcome.kind = ReviewOutcome::Kind::Unavailable;
        outcome.note = error;
        return outcome;
    }
    const CodeWindow window = ExtractWindow(job.file, content, job.anchors, job.startLine,
        static_cast<std::size_t>(current.windowLines), MaximumWindowCharacters);
    outcome.nextLine = window.lastLine >= window.totalLines ? 1 : window.lastLine + 1;
    Log("Reviewing " + job.file + " lines " + std::to_string(window.firstLine) + "-" +
        std::to_string(window.lastLine) + " (" + job.trigger + ").");

    const responseOutput reply = use.review(
        Instructions(job, use.store->Lessons(6)), Material(window), Schema(), stopToken);
    if (!reply.bSuccess)
    {
        outcome.kind = ReviewOutcome::Kind::Unavailable;
        outcome.note = reply.reason.empty() ? "The review did not come back." : reply.reason;
        return outcome;
    }
    std::string note;
    std::optional<CodeProposal> proposal = ParseReviewReply(reply.response, note);
    const std::string where = job.file + " " + std::to_string(window.firstLine) + "-" +
        std::to_string(window.lastLine);
    if (!proposal)
    {
        outcome.kind = ReviewOutcome::Kind::Nothing;
        outcome.note = note;
        Log("Nothing in " + where + ": " + Short(note, 200) + " | reply: " +
            Short(reply.response, 400));
        remember("Reviewed " + where + ": nothing worth changing.");
        return outcome;
    }

    const auto refuse = [&](const std::string& why)
    {
        outcome.kind = ReviewOutcome::Kind::Refused;
        outcome.note = why;
        Log("Set aside \"" + Short(proposal->title, 80) + "\" in " + where + ": " + why);
        remember("Reviewed " + where + ": set aside an idea (" + Short(why, 80) + ").");
        return outcome;
    };
    // She was shown one file; an edit to another was written blind. Windows paths ignore
    // case, and she wrote LLamaCppServerProcess.cpp for llamaCppServerProcess.cpp once.
    const auto sameIgnoringCase = [](const std::string& left, const std::string& right)
    {
        return left.size() == right.size() && std::equal(left.begin(), left.end(),
            right.begin(), [](const unsigned char a, const unsigned char b)
            {
                return std::tolower(a) == std::tolower(b);
            });
    };
    if (!sameIgnoringCase(proposal->change.path, job.file))
        return refuse("It edits " + proposal->change.path + ", which she was not shown.");
    proposal->change.path = job.file;
    ChangeCheck check = CheckChange(proposal->change, content);
    if (!check.ok && check.notFound && !stopToken.stop_requested())
    {
        // Seen live: she shortens the lines she means with "...", so nothing matches.
        // One more look, told exactly what went wrong, before the idea is set aside.
        const std::string stray = FirstLineNotInFile(content, proposal->change.find);
        Log(stray.empty()
            ? "The lines she meant to replace are all in the file, but not together once: " +
                Short(proposal->change.find, 240)
            : "She wrote a line to replace that is not in the file: " + Short(stray, 240));
        std::ostringstream retry;
        retry << Instructions(job, use.store->Lessons(6))
              << "\n\nYou proposed a change, but the text you gave in \"find\" is not in the "
                 "file as written:\n" << proposal->change.find;
        if (!stray.empty())
        {
            retry << "\nThis line of it is not in the code at all: " << stray;
        }
        retry << "\nCopy the lines you mean from the code shown, every character, nothing "
                 "shortened and no \"...\". Return the same change with a corrected find, or "
                 "found=false.";
        const responseOutput again =
            use.review(retry.str(), Material(window), Schema(), stopToken);
        std::string retryNote;
        std::optional<CodeProposal> corrected = again.bSuccess
            ? ParseReviewReply(again.response, retryNote) : std::nullopt;
        if (corrected && sameIgnoringCase(corrected->change.path, job.file))
        {
            corrected->change.path = job.file;
            const ChangeCheck recheck = CheckChange(corrected->change, content);
            if (recheck.ok)
            {
                Log("She copied the text to replace correctly on a second look.");
                proposal = std::move(corrected);
                check = recheck;
            }
        }
    }
    if (!check.ok) return refuse(check.reason);
    if (use.store->Known(proposal->fingerprint)) return refuse("She proposed this exact change before.");
    // Seen live: "Handle 'sing' as a valid command" and then "Handle 'sing' as a valid
    // request", the same lines, while the first still waited for a decision. A second
    // idea about code with one already open waits for the verdict on the first.
    for (const CodeProposal& earlier : use.store->All())
    {
        const bool open = earlier.status == ProposalStatus::Drafted ||
            earlier.status == ProposalStatus::Verified;
        if (open && Overlaps(content, earlier.change, proposal->change))
        {
            return refuse("Proposal #" + earlier.id + " already changes this code and is "
                "waiting for a decision.");
        }
    }
    const double bar = job.trigger == "exploration" ? current.minimumBenefit
        : job.trigger == "evidence" ? current.evidenceMinimumBenefit
        : std::min(0.3, current.evidenceMinimumBenefit);
    char scores[96] = {};
    std::snprintf(scores, sizeof(scores), "benefit %.2f (needs %.2f), risk %.2f (at most %.2f)",
        proposal->benefit, bar, proposal->risk, current.maximumRisk);
    if (proposal->benefit < bar || proposal->risk > current.maximumRisk)
        return refuse(std::string("Not worth it by her own estimate: ") + scores + ".");

    proposal->trigger = job.trigger;
    proposal->taskId = job.taskId;
    proposal->createdAt = std::to_string(NowEpoch());
    proposal->status = ProposalStatus::Drafted;
    if (!use.store->Save(*proposal, MakeUnifiedDiff(content, proposal->change), error))
    {
        outcome.kind = ReviewOutcome::Kind::Unavailable;
        outcome.note = error;
        return outcome;
    }
    Log("Proposal #" + proposal->id + " \"" + Short(proposal->title, 100) + "\" in " +
        proposal->change.path + " (" + scores + ").");

    if (current.bVerify && use.workbench)
    {
        Prove(*proposal, job, content, stopToken);
    }
    else if (use.report)
    {
        use.report(*proposal, "I think I found an improvement in " + proposal->change.path +
            ": " + proposal->title + ". I haven't proven it builds (proving is off). "
            "/improve show " + proposal->id);
    }
    outcome.kind = ReviewOutcome::Kind::Proposed;
    outcome.proposal = proposal;
    remember("Proposal #" + proposal->id + " (" + ToString(proposal->status) + "): " +
        Short(proposal->title, 80));
    return outcome;
}

void ImprovementAgent::Prove(
    CodeProposal& proposal, const ReviewJob& job, const std::string& original,
    const std::stop_token stopToken)
{
    improvementSettings current;
    Dependencies use;
    {
        std::lock_guard lock(mutex);
        current = settings;
        use = dependencies;
    }
    const auto save = [&]()
    {
        std::string error;
        (void)use.store->Save(proposal, MakeUnifiedDiff(original, proposal.change), error);
    };

    // A build takes much of the machine for minutes, so one she decided on alone waits for
    // a long quiet spell and stops the moment someone is back. One asked for runs now.
    const bool asked = job.trigger == "request";
    std::stop_source proof;
    std::stop_callback forward(stopToken, [&proof]() { proof.request_stop(); });
    std::jthread watcher;
    if (!asked)
    {
        const int quiet = std::max(1, current.idleMinutesBeforeBuilding) * 60;
        if (!use.idle || !use.idle(quiet, true))
        {
            proposal.verificationSummary = "Waiting for a quiet moment to build and test it.";
            save();
            return;
        }
        watcher = std::jthread([&proof, idle = use.idle](const std::stop_token watching)
        {
            while (!watching.stop_requested() && !proof.stop_requested())
            {
                for (int tick = 0; tick < 10 && !watching.stop_requested(); ++tick)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!watching.stop_requested() && !idle(30, false)) proof.request_stop();
            }
        });
    }

    Log("Proving #" + proposal.id + " in the workbench.");
    VerificationResult result = use.workbench->Verify(proposal.change, proof.get_token());

    // One repair, shown the compiler's own words. A small model's first edit often misses
    // an include or a type; the second, told exactly what, often does not.
    if (result.concluded && !result.verified && !result.buildErrors.empty() && use.review &&
        !proof.stop_requested())
    {
        const CodeWindow window = ExtractWindow(proposal.change.path, original,
            {proposal.change.find.substr(0, proposal.change.find.find('\n'))}, 1,
            static_cast<std::size_t>(current.windowLines), MaximumWindowCharacters);
        std::ostringstream repair;
        repair << Instructions(job, {}) << "\n\nYou already proposed this change to the code "
               << "below, and it did not compile.\nYour find:\n" << proposal.change.find
               << "\nYour replace:\n" << proposal.change.replace << "\nCompiler errors:\n"
               << result.buildErrors
               << "\nReturn a corrected change against the ORIGINAL code shown (copy find from "
                  "it), or found=false if it cannot be fixed simply.";
        const responseOutput reply = use.review(repair.str(), Material(window), Schema(), proof.get_token());
        std::string note;
        const std::optional<CodeProposal> fixed =
            reply.bSuccess ? ParseReviewReply(reply.response, note) : std::nullopt;
        if (fixed && fixed->change.path == proposal.change.path &&
            CheckChange(fixed->change, original).ok && !use.store->Known(fixed->fingerprint))
        {
            Log("Repairing #" + proposal.id + " after build errors.");
            proposal.change = fixed->change;
            proposal.fingerprint = fixed->fingerprint;
            if (!fixed->reason.empty()) proposal.reason = fixed->reason;
            result = use.workbench->Verify(proposal.change, proof.get_token());
            if (result.verified) result.summary += " (after one repair)";
        }
    }
    if (watcher.joinable())
    {
        watcher.request_stop();
        watcher.join();
    }

    if (!result.concluded)
    {
        proposal.verificationSummary = "Not yet proven: " + result.summary;
        save();
        Log("#" + proposal.id + " not proven yet: " + result.summary);
        return;
    }
    proposal.status = result.verified ? ProposalStatus::Verified : ProposalStatus::FailedVerification;
    proposal.verificationSummary = result.summary;
    save();
    Log("#" + proposal.id + " " + ToString(proposal.status) + ": " + result.summary);
    if (result.verified && use.report)
    {
        use.report(proposal, "I found an improvement to my own code in " + proposal.change.path +
            ": " + proposal.title + ". " + result.summary + " Why: " +
            Short(proposal.reason, 220) + " /improve show " + proposal.id);
    }
}

std::optional<ReviewJob> ImprovementAgent::NextJob()
{
    improvementSettings current;
    Dependencies use;
    {
        std::lock_guard lock(mutex);
        if (!requests.empty())
        {
            ReviewJob job = requests.front();
            requests.pop_front();
            return job;
        }
        current = settings;
        use = dependencies;
    }
    if (!current.bEnabled || !use.store || !use.catalog.Valid() || !use.idle) return std::nullopt;
    // Don't bury the person: new work waits while earlier proven proposals do.
    if (use.store->AwaitingDecision() >= static_cast<std::size_t>(std::max(1, current.maximumAwaitingDecision)))
        return std::nullopt;

    // Proposals recorded but not yet proven, oldest first.
    if (current.bVerify && use.workbench &&
        use.idle(std::max(1, current.idleMinutesBeforeBuilding) * 60, true))
    {
        const std::vector<CodeProposal> all = use.store->All();
        const auto now = std::chrono::steady_clock::now();
        for (auto proposal = all.rbegin(); proposal != all.rend(); ++proposal)
        {
            bool recentlyTried = false;
            {
                std::lock_guard lock(mutex);
                const auto tried = lastProofAttempt.find(proposal->id);
                // A proof that could not finish -- no toolchain, a build past its time
                // limit -- is not retried every minute.
                recentlyTried = tried != lastProofAttempt.end() &&
                    now - tried->second < std::chrono::minutes(30);
            }
            if (proposal->status == ProposalStatus::Drafted && !recentlyTried)
            {
                ReviewJob job;
                job.trigger = "prove";
                job.taskId = proposal->id;
                return job;
            }
        }
    }
    if (!use.idle(ReviewQuietSeconds, true)) return std::nullopt;

    // A measured problem first: that is a weakness known to be real.
    if (use.tasks)
    {
        for (const learning::SelfImprovementTask& task : use.tasks())
        {
            if (task.id.empty() || use.store->TaskReviewed(task.id)) continue;
            if (use.store->TaskReviewed(CategoryWeekKey(task.category)))
            {
                use.store->MarkTaskReviewed(task.id);
                continue;
            }
            const std::vector<std::string> files =
                use.catalog.FilesFor(task.relatedComponents, task.relatedMetrics, 1);
            if (files.empty())
            {
                use.store->MarkTaskReviewed(task.id);
                Log("No code found for task " + task.id + " (" + task.category + ").");
                continue;
            }
            ReviewJob job;
            job.trigger = "evidence";
            job.taskId = task.id;
            job.category = task.category;
            job.problem = task.observedProblem;
            job.evidence = task.evidence;
            job.anchors = task.relatedMetrics;
            job.file = files.front();
            return job;
        }
    }

    if (current.bExplore)
    {
        const std::int64_t since = NowEpoch() - use.store->LastExplorationEpoch();
        if (since >= static_cast<std::int64_t>(std::max(1, current.explorationIntervalMinutes)) * 60)
        {
            const std::string file = use.store->LeastRecentlyExplored(use.catalog.Files());
            if (!file.empty())
            {
                ReviewJob job;
                job.trigger = "exploration";
                job.file = file;
                job.startLine = use.store->NextLine(file);
                return job;
            }
        }
    }
    return std::nullopt;
}

void ImprovementAgent::Loop(const std::stop_token stopToken)
{
    // Nothing thrown here may leave this thread. An exception escaping a std::jthread
    // body ends the process, and a review that failed -- a file the model cannot be sent,
    // a store that cannot be written -- is an ordinary outcome that waits and tries later.
    const auto failed = [](const std::string& why)
    {
        ReviewOutcome outcome;
        outcome.kind = ReviewOutcome::Kind::Unavailable;
        outcome.note = why;
        return outcome;
    };
    while (!stopToken.stop_requested())
    {
        std::optional<ReviewJob> job;
        try
        {
            job = NextJob();
        }
        catch (const std::exception& error)
        {
            Log(std::string("Choosing what to review failed: ") + error.what());
        }
        catch (...)
        {
            Log("Choosing what to review failed without saying why.");
        }
        if (!job)
        {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stopToken, std::chrono::seconds(60),
                [this]() { return !requests.empty(); });
            continue;
        }
        ReviewOutcome outcome;
        try
        {
            outcome = Run(*job, stopToken);
        }
        catch (const std::exception& error)
        {
            outcome = failed(std::string("The review failed: ") + error.what());
        }
        catch (...)
        {
            outcome = failed("The review failed without saying why.");
        }
        std::shared_ptr<ProposalStore> store;
        {
            std::lock_guard lock(mutex);
            store = dependencies.store;
        }
        if (store && outcome.kind != ReviewOutcome::Kind::Unavailable)
        {
            if (job->trigger == "evidence")
            {
                store->MarkTaskReviewed(job->taskId);
                store->MarkTaskReviewed(CategoryWeekKey(job->category));
            }
            if (job->trigger == "exploration")
                store->MarkExplored(job->file, outcome.nextLine, NowEpoch());
        }
        if (outcome.kind == ReviewOutcome::Kind::Unavailable)
        {
            Log("Review unavailable: " + outcome.note);
            // An exploration that could not run still counts, or a missing model would
            // retry the same window every minute.
            if (store && job->trigger == "exploration")
                store->MarkExplored(job->file, job->startLine, NowEpoch());
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stopToken, std::chrono::seconds(300),
                [this]() { return !requests.empty(); });
        }
    }
}

} // namespace revia::improvement
