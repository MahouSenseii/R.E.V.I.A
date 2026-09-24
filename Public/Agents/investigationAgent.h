#pragma once

#include "Agents/investigation.h"
#include "Core/messageRouter.h"
#include "Library/structLibrary.h"

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

// The model-backed investigator.
//
// Reuses the same bounded-deliberation path SelfInquiryAgent uses -- one router, one
// posture, one call per round -- and adds the only thing the single pass could not do:
// it is handed what the previous round actually observed, so the next question can be
// chosen because of it.
//
// The integrity rule this class exists to enforce is narrow and absolute. A model can
// write the words "I ran the tests" as easily as anything else. Unless a real executor
// performed a real check, whatever comes back is the model's interpretation and is
// labelled as such. There is no path here by which generated text becomes a tool
// observation.

// Performs one proposed check through the existing permission, approval, cancellation and
// audit paths. Supplied by the runtime when real checks are available.
//
// Returning false means the check did not run -- refused, unavailable, or unsupported --
// and the question becomes Blocked rather than answered.
struct ExecutedCheck
{
    bool ran = false;
    // What the check actually observed. Empty when it did not run.
    std::string observed;
    std::string limitations;
    // Why it did not run, when it did not.
    std::string refusal;
};

using CheckExecutor = std::function<ExecutedCheck(
    CheckKind kind, const std::string& description, const std::string& questionText)>;

class InvestigationAgent
{
public:
    // The envelope for one round. Carries the goal, the questions being checked now, and
    // the findings from every earlier round.
    //
    // Static and pure so the schema can be tested without a model or a socket, which is
    // the same reason SelfInquiryAgent exposes its envelope.
    [[nodiscard]] static std::string BuildRoundEnvelope(
        const RoundRequest& request,
        const std::string& identityPosture,
        bool checksAreAvailable);

    // Parses one round's reply.
    //
    // `checksAreAvailable` decides what a claimed check kind is allowed to become. With
    // no executor wired, every claim of having consulted something is downgraded to
    // reasoning, because nothing was consulted.
    [[nodiscard]] static RoundResult ParseRound(
        const std::string& raw,
        const RoundRequest& request,
        bool checksAreAvailable);

    // Builds the runner the loop drives. `executor` may be empty, in which case the round
    // is reasoning only and says so.
    [[nodiscard]] static RoundRunner MakeRunner(
        const messageRouter& router,
        std::string identityPosture,
        CheckExecutor executor = {},
        std::stop_token stopToken = {});

    // The opening questions, derived from the goal in one bounded call. Separate from the
    // rounds so an investigation that cannot even be started fails before any round runs.
    [[nodiscard]] static std::vector<ProposedQuestion> ParseOpeningQuestions(
        const std::string& raw, std::size_t maximumQuestions);

    [[nodiscard]] static std::string BuildOpeningEnvelope(
        const std::string& goal,
        const std::string& identityPosture,
        const std::vector<conversationMessage>& context,
        std::size_t maximumQuestions);
};

} // namespace revia::agents
