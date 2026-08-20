#include "Initiative/initiativeController.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace revia::initiative
{

namespace
{

std::string FormatMinutes(const std::chrono::seconds value)
{
    const std::int64_t minutes = value.count() / 60;
    if (minutes < 60)
    {
        return std::to_string(minutes) + " minutes";
    }
    return std::to_string(minutes / 60) + "h" + std::to_string(minutes % 60) + "m";
}

} // namespace

void InitiativeController::Configure(initiativeSettings settings)
{
    std::lock_guard lock(mutex);
    configuration = settings;
    policy = AttentionPolicy(std::move(settings));
    proposals.clear();
    lastSubject.clear();
    nextId = 1;
}

bool InitiativeController::BuildActivityProposal(
    const std::vector<perception::ActivitySpan>& recent,
    Proposal& outProposal)
{
    if (recent.empty())
    {
        return false;
    }

    // Total per application, so a long stretch of real work is distinguished from a lot
    // of switching between things.
    std::vector<std::pair<std::string, std::chrono::seconds>> totals;
    std::chrono::seconds overall{0};
    for (const perception::ActivitySpan& span : recent)
    {
        overall += span.Duration();
        const auto existing = std::find_if(
            totals.begin(),
            totals.end(),
            [&span](const auto& entry) { return entry.first == span.application; });
        if (existing != totals.end())
        {
            existing->second += span.Duration();
        }
        else
        {
            totals.emplace_back(span.application, span.Duration());
        }
    }
    if (totals.empty() || overall < std::chrono::minutes{20})
    {
        return false;
    }

    std::sort(totals.begin(), totals.end(), [](const auto& left, const auto& right)
    {
        return left.second > right.second;
    });
    const auto& dominant = totals.front();
    if (dominant.second < std::chrono::minutes{20})
    {
        return false;
    }

    // Confidence comes from how concentrated the session is, not from a model's opinion
    // of its own interestingness. A session split across six applications is not evidence
    // of anything worth interrupting for.
    const float share = static_cast<float>(dominant.second.count()) /
        static_cast<float>(std::max<std::int64_t>(1, overall.count()));
    outProposal.confidence = std::clamp(share, 0.0f, 1.0f);

    std::vector<std::string> subjects;
    for (const perception::ActivitySpan& span : recent)
    {
        if (span.application != dominant.first)
        {
            continue;
        }
        for (const std::string& title : span.titles)
        {
            if (subjects.size() < 3 &&
                std::find(subjects.begin(), subjects.end(), title) == subjects.end())
            {
                subjects.push_back(title);
            }
        }
    }

    std::ostringstream evidence;
    evidence << FormatMinutes(dominant.second) << " in " << dominant.first;
    if (!subjects.empty())
    {
        evidence << " (";
        for (std::size_t index = 0; index < subjects.size(); ++index)
        {
            evidence << (index == 0 ? "" : ", ") << subjects[index];
        }
        evidence << ")";
    }
    evidence << ", " << static_cast<int>(share * 100.0f) << "% of the observed session";
    outProposal.evidence = evidence.str();

    std::ostringstream message;
    message << "You have been in " << dominant.first << " for "
        << FormatMinutes(dominant.second);
    if (!subjects.empty())
    {
        message << ", mostly " << subjects.front();
    }
    message << ". Want me to do anything with that?";
    outProposal.message = message.str();
    return true;
}

bool InitiativeController::BuildUnfinishedGoalProposal(
    const std::vector<goals::Goal>& unfinishedGoals,
    Proposal& outProposal)
{
    // The most advanced one: a goal that got most of the way through is both the most
    // useful to finish and the least likely to have been abandoned deliberately.
    const goals::Goal* best = nullptr;
    for (const goals::Goal& goal : unfinishedGoals)
    {
        if (goals::IsTerminal(goal.status) || goal.steps.empty())
        {
            continue;
        }
        if (best == nullptr || goal.currentStep > best->currentStep)
        {
            best = &goal;
        }
    }
    if (best == nullptr)
    {
        return false;
    }

    // High and fixed. This is not an inference about what the user might want: a goal
    // they started and did not finish is a fact, and finishing it was already the
    // declared intent.
    outProposal.confidence = 0.95f;
    outProposal.resumeGoalId = best->id;

    std::ostringstream evidence;
    evidence << "goal '" << best->title << "' stopped at step " << best->currentStep
        << " of " << best->steps.size();
    if (best->stopReason != goals::StopReason::None &&
        best->stopReason != goals::StopReason::Completed)
    {
        evidence << " (" << goals::ToString(best->stopReason) << ")";
    }
    outProposal.evidence = evidence.str();

    std::ostringstream message;
    message << "You left '" << best->title << "' unfinished at step "
        << best->currentStep << " of " << best->steps.size()
        << ". Want me to pick it up?";
    outProposal.message = message.str();
    return true;
}

bool InitiativeController::BuildConversationProposal(
    const std::vector<StarterCue>& cues,
    Proposal& outProposal)
{
    if (cues.empty())
    {
        return false;
    }
    const auto strongest = std::max_element(
        cues.begin(),
        cues.end(),
        [](const StarterCue& left, const StarterCue& right)
        {
            if (left.confidence != right.confidence)
            {
                return left.confidence < right.confidence;
            }
            return left.occurredAt < right.occurredAt;
        });
    outProposal.kind = Proposal::Kind::ConversationStarter;
    outProposal.message = strongest->messageIntent;
    outProposal.evidence = strongest->evidence;
    outProposal.confidence = strongest->confidence;
    return true;
}

bool InitiativeController::BuildProposal(const Evidence& evidence, Proposal& outProposal)
{
    // Ordered by how concrete the evidence is, not by how interesting it sounds. An
    // unfinished goal is something the user actually asked for; a busy session is only an
    // observation about it, so it never displaces one.
    if (BuildUnfinishedGoalProposal(evidence.unfinishedGoals, outProposal))
    {
        return true;
    }
    if (BuildConversationProposal(evidence.conversationCues, outProposal))
    {
        return true;
    }
    return BuildActivityProposal(evidence.recentActivity, outProposal);
}

InitiativeController::Consideration InitiativeController::Consider(
    const Evidence& evidence,
    const AttentionContext& context)
{
    std::lock_guard lock(mutex);
    Consideration consideration;

    Proposal candidate;
    if (!BuildProposal(evidence, candidate))
    {
        consideration.verdict = AttentionVerdict::BelowConfidence;
        return consideration;
    }

    consideration.verdict = policy.Evaluate(candidate.confidence, context);
    if (IsSuppression(consideration.verdict))
    {
        policy.RecordSuppressed();
        return consideration;
    }

    // Do not say the same thing twice. Repeating an observation the user already declined
    // to act on is the fastest way to become noise.
    if (!lastSubject.empty() && candidate.evidence == lastSubject)
    {
        consideration.verdict = AttentionVerdict::BelowConfidence;
        policy.RecordSuppressed();
        return consideration;
    }

    candidate.id = "proposal-" + std::to_string(nextId++);
    candidate.createdAt = context.now;
    lastSubject = candidate.evidence;
    policy.RecordSpoken(context.now);
    proposals.emplace_back(candidate, ProposalOutcome::Pending);

    consideration.hasProposal = true;
    consideration.proposal = candidate;
    return consideration;
}

void InitiativeController::Accept(const std::string& proposalId)
{
    std::lock_guard lock(mutex);
    for (auto& entry : proposals)
    {
        if (entry.first.id == proposalId && entry.second == ProposalOutcome::Pending)
        {
            entry.second = ProposalOutcome::Accepted;
            policy.RecordAccepted();
            return;
        }
    }
}

void InitiativeController::RecordConversationResponse(
    const std::string& response,
    const std::chrono::system_clock::time_point when)
{
    std::string normalized;
    normalized.reserve(response.size());
    for (const unsigned char character : response)
    {
        if (std::isalnum(character) != 0 || std::isspace(character) != 0)
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    constexpr std::string_view Dismissals[] = {
        "not now", "no thanks", "maybe later", "leave me alone", "dont interrupt",
        "do not interrupt", "stop interrupting", "quiet please"
    };
    const bool dismissed = std::any_of(std::begin(Dismissals), std::end(Dismissals),
        [&normalized](const std::string_view phrase)
        {
            return normalized.find(phrase) != std::string::npos;
        });

    std::lock_guard lock(mutex);
    for (auto& entry : proposals)
    {
        if (entry.second == ProposalOutcome::Pending &&
            entry.first.kind == Proposal::Kind::ConversationStarter)
        {
            if (dismissed)
            {
                entry.second = ProposalOutcome::Dismissed;
                policy.RecordDismissed(when);
            }
            else
            {
                entry.second = ProposalOutcome::Accepted;
                policy.RecordAccepted();
            }
        }
    }
}

void InitiativeController::Dismiss(
    const std::string& proposalId,
    const std::chrono::system_clock::time_point when)
{
    std::lock_guard lock(mutex);
    for (auto& entry : proposals)
    {
        if (entry.first.id == proposalId && entry.second == ProposalOutcome::Pending)
        {
            entry.second = ProposalOutcome::Dismissed;
            policy.RecordDismissed(when);
            return;
        }
    }
}

void InitiativeController::Expire(const std::string& proposalId)
{
    std::lock_guard lock(mutex);
    for (auto& entry : proposals)
    {
        if (entry.first.id == proposalId && entry.second == ProposalOutcome::Pending)
        {
            entry.second = ProposalOutcome::Expired;
            return;
        }
    }
}

std::vector<Proposal> InitiativeController::Pending() const
{
    std::lock_guard lock(mutex);
    std::vector<Proposal> pending;
    for (const auto& entry : proposals)
    {
        if (entry.second == ProposalOutcome::Pending)
        {
            pending.push_back(entry.first);
        }
    }
    return pending;
}

InitiativeCounters InitiativeController::Counters() const
{
    std::lock_guard lock(mutex);
    return policy.Counters();
}

float InitiativeController::Precision() const
{
    std::lock_guard lock(mutex);
    return policy.Precision();
}

bool InitiativeController::IsRateReduced() const
{
    std::lock_guard lock(mutex);
    return policy.IsRateReduced();
}

std::string InitiativeController::Status() const
{
    std::lock_guard lock(mutex);
    const InitiativeCounters counters = policy.Counters();
    std::ostringstream stream;
    if (!configuration.bEnabled)
    {
        stream << "Revia never speaks first. Enable \"initiative\" in Config/settings.json.";
        return stream.str();
    }
    stream << "Revia may speak first, at most " << policy.EffectiveHourlyBudget()
        << " times an hour";
    if (policy.IsRateReduced())
    {
        stream << " (reduced automatically: too many proposals were dismissed)";
    }
    stream << ".\nSpoken " << counters.spoken << ", accepted " << counters.accepted
        << ", dismissed " << counters.dismissed << ", suppressed " << counters.suppressed
        << ".";
    const std::uint32_t judged = counters.accepted + counters.dismissed;
    if (judged >= static_cast<std::uint32_t>(std::max(1, configuration.precisionSampleFloor)))
    {
        stream << "\nPrecision " << static_cast<int>(policy.Precision() * 100.0f) << "%.";
    }
    else
    {
        stream << "\nPrecision not yet measurable (" << judged << " judged).";
    }
    return stream.str();
}

} // namespace revia::initiative
