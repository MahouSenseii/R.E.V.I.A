#pragma once

#include "Agents/memoryAgent.h"
#include "Autonomy/activity.h"
#include "Autonomy/driveState.h"

#include <optional>
#include <string>
#include <vector>

namespace revia::autonomy
{

// What executing one activity produced.
//
// Separate from Activity because an activity is a decision and this is what came of it.
// The status here is the real lifecycle outcome, including Interrupted -- which is not a
// failure and must never be recorded as one, or every time the user speaks Revia learns
// that what she was doing did not work.
struct ActivityOutcome
{
    ActivityStatus status = ActivityStatus::Failed;
    // Plain sentence for the activity feed and the log. Says what actually happened,
    // not what was attempted.
    std::string summary;
    // Enough to pick the work back up. Set when the status is Interrupted or Waiting,
    // empty otherwise -- a terminal activity with a resume token invites a resume that
    // would repeat finished work.
    std::string resumeToken;
    // Whether the activity did enough to spend the drive that motivated it. An activity
    // that was interrupted before doing anything must not satisfy a drive, or being
    // interrupted would make Revia stop wanting what she never got to do.
    bool satisfiedDrive = false;
    std::optional<Drive> drive;
    // Set when the activity produced something durable: a memory, a note, a diagram.
    // Reported so "she did something" and "she has something to show for it" stay
    // different claims.
    std::string artifact;
};

// The drive an activity spends when it genuinely completes.
//
// Nothing spends nothing: declining to act is not a way of satisfying a want, and
// treating it as one would make a quiet Revia progressively less motivated.
[[nodiscard]] std::optional<Drive> DriveSatisfiedBy(ActivityType type);

// Whether a candidate string is a research topic or a command that was mistaken for one.
//
// This gate exists because of a real failure: the raw utterance "look up what you want !"
// was sent verbatim to the search backend, which returned a Poison album, a Beatles song
// and an Evanescence song, and that text was then fed back as grounding. Delegating the
// choice of topic is not a topic, and searching the delegation is worse than not
// searching at all -- it produces confident, irrelevant evidence.
struct ResearchTopicVerdict
{
    bool usable = false;
    // The topic with any leading research verb removed, so "read about tail latency"
    // becomes "tail latency". Empty when the verdict refuses.
    std::string topic;
    // Why it was refused, in plain language. Always populated on a refusal.
    std::string refusal;
    // True when the candidate delegated the choice rather than naming a subject
    // ("whatever you want"). The caller answers this by choosing a topic from its own
    // context or by declining -- never by searching the delegation.
    bool delegated = false;
};

[[nodiscard]] ResearchTopicVerdict ResolveResearchTopic(const std::string& candidate);

// Picks a research topic from what Revia already has, when the request did not name one.
//
// Candidates are offered in priority order and the first usable one wins. Returning
// nothing is a normal outcome and means the correct action is to not research: a topic
// invented to satisfy a drive is exactly the random behaviour autonomy is supposed to
// exclude.
[[nodiscard]] ResearchTopicVerdict ChooseResearchTopic(
    const std::vector<std::string>& candidatesInPriorityOrder);

// A bounded, filesystem-safe name for something Revia made on her own.
//
// Autonomous creation writes only inside her own workspace, and a title that came from a
// model is not a filename. This strips it to a known-safe shape rather than trusting it.
[[nodiscard]] std::string WorkspaceArtifactName(
    const std::string& title,
    const std::string& extension);

// What an activity should report as ActivityOutcome::artifact for a learned finding it
// submitted, given how that submission actually resolved.
//
// `kept` is used when the finding genuinely reached durable memory -- saved, whether or
// not its embedding has landed yet, or already there from an earlier pass -- and
// `pending` while it is still only queued behind other work, not yet durable. Nothing is
// returned for a submission that failed outright: an empty artifact leaves the outcome
// message honest instead of claiming a save that never happened. This is the one place
// that mapping happens, so every autonomous activity reports the same disposition the
// same way.
[[nodiscard]] std::string DescribeLearnedFindingArtifact(
    agents::LearnedFindingResult result,
    const std::string& kept,
    const std::string& pending);

} // namespace revia::autonomy
