#include "testSupport.h"

#include "Intelligence/intelligenceRouter.h"
#include "Runtime/conversationRuntime.h"

#include <iostream>
#include <string>

// Routing, exercised through the function production uses to build the router's input
// rather than by handing the router a context a test filled in itself.
//
// A suite that writes `context.suppliedFileCount = 3; router.Route(...)` proves the
// router branches on the field. It proves nothing about whether any request can ever
// set it -- and for three of these fields, none could. That gap is what this file
// exists to close: every case starts from what a turn actually knows and ends at a
// tier.
namespace
{
using revia::intelligence::IntelligenceTier;
using revia::intelligence::ReasoningMode;
using revia::intelligence::RoutingContext;
using revia::runtime::BuildRoutingContext;
using revia::runtime::RoutingInputs;
using revia::tests::Check;

// Fails to compile if RoutingContext gains or loses a field.
//
// Not a style check. Each name below is a routing input with a named producer in
// BuildRoutingContext, and the point of binding all of them is that adding a sixth
// input breaks this line, so somebody has to say where its value comes from before the
// router can branch on it. Three fields reached production without anyone being asked
// that question.
void TestEveryRoutingInputHasAProducer()
{
    RoutingContext context;
    auto& [visionRequired, expertVisionPreferred, explicitResearch, previousUncertainty,
           recentContextCharacters, previousAssistantTier] = context;
    static_cast<void>(visionRequired);
    static_cast<void>(expertVisionPreferred);
    static_cast<void>(explicitResearch);
    static_cast<void>(previousUncertainty);
    static_cast<void>(recentContextCharacters);
    static_cast<void>(previousAssistantTier);

    // And each one is really reachable from a turn's own state.
    RoutingInputs screen;
    screen.input = "What is on my screen right now?";
    Check(BuildRoutingContext(screen).visionRequired,
        "A screen question did not set visionRequired.");

    RoutingInputs blueprint;
    blueprint.input = "Look at my screen and explain this blueprint graph.";
    Check(BuildRoutingContext(blueprint).expertVisionPreferred,
        "A visual architecture question did not set expertVisionPreferred.");

    RoutingInputs research;
    research.input = "Look up the latest Qt release online.";
    Check(BuildRoutingContext(research).explicitResearch,
        "An internet question did not set explicitResearch.");

    RoutingInputs researchDenied = research;
    researchDenied.allowInternetLookup = false;
    Check(!BuildRoutingContext(researchDenied).explicitResearch,
        "A turn that may not look anything up was still marked as research.");

    RoutingInputs longContext;
    longContext.input = "And then?";
    longContext.recentContextCharacters = 9000;
    Check(BuildRoutingContext(longContext).recentContextCharacters == 9000,
        "The turn's context size did not reach the router.");

    RoutingInputs continued;
    continued.input = "Why?";
    continued.previousDeliveredTier = IntelligenceTier::Expert;
    Check(BuildRoutingContext(continued).previousAssistantTier ==
            IntelligenceTier::Expert,
        "The previous delivered tier did not reach the router.");

    RoutingInputs doubted;
    doubted.input = "Try that again.";
    doubted.previousTurnWasUnreliable = true;
    Check(BuildRoutingContext(doubted).previousUncertainty,
        "The previous turn's recorded outcome did not reach the router.");
}

// The whole path, from a request to a tier, for each condition the evaluation corpus
// names.
void TestARealRequestReachesTheExpectedTier()
{
    const revia::intelligence::IntelligenceRouter router;
    const auto route = [&router](const RoutingInputs& inputs)
    {
        return router.Route(inputs.input, BuildRoutingContext(inputs));
    };

    // A previously unreliable answer. This is the case that had no producer at all: the
    // router escalated on previousUncertainty and nothing in the runtime ever set it.
    RoutingInputs afterBadAnswer;
    afterBadAnswer.input = "So what should I do about it?";
    afterBadAnswer.previousTurnWasUnreliable = true;
    const auto escalated = route(afterBadAnswer);
    Check(escalated.selectedTier == IntelligenceTier::Expert &&
        escalated.mode == ReasoningMode::Deep,
        "A turn following an answer the runtime itself distrusted did not escalate.");
    Check(escalated.reason.find("previous answer") != std::string::npos,
        "The escalation did not say why it escalated.");

    // The same words after a good answer stay ordinary. Without this the case above
    // would pass for a router that escalated everything.
    RoutingInputs afterGoodAnswer = afterBadAnswer;
    afterGoodAnswer.previousTurnWasUnreliable = false;
    Check(route(afterGoodAnswer).selectedTier != IntelligenceTier::Expert,
        "An ordinary follow-up escalated to Expert with no reason to.");

    // A doubted answer to a visual question reaches the Expert projector by the same
    // input, which is the other branch that had no producer.
    RoutingInputs doubtedVisual;
    doubtedVisual.input = "Can you see my screen now?";
    doubtedVisual.previousTurnWasUnreliable = true;
    Check(route(doubtedVisual).selectedTier == IntelligenceTier::ExpertVision,
        "A re-checked visual question after a distrusted answer did not reach the "
        "Expert projector.");

    // A screen-dependent question.
    RoutingInputs screen;
    screen.input = "What is on my screen?";
    Check(route(screen).selectedTier == IntelligenceTier::Vision,
        "A screen question did not route to a vision tier.");

    // The same question when the turn is not allowed screen context. The permission is
    // part of the turn, so it belongs in the routing decision rather than being
    // discovered afterwards.
    RoutingInputs screenDenied = screen;
    screenDenied.allowScreenContext = false;
    Check(route(screenDenied).selectedTier != IntelligenceTier::Vision,
        "A vision tier was selected for a turn that may not look at the screen.");

    // A fresh internet-dependent question.
    RoutingInputs research;
    research.input = "Look things up online: what is the current Qt LTS version?";
    const auto researched = route(research);
    Check(researched.selectedTier == IntelligenceTier::Main &&
        researched.mode == ReasoningMode::Deep,
        "A research question did not route to Main with deep reasoning.");

    // Grounding the runtime already fetched is evidence on its own, whatever the
    // sentence looked like.
    RoutingInputs grounded;
    grounded.input = "And what did that say?";
    grounded.groundingAlreadyRetrieved = true;
    Check(route(grounded).mode == ReasoningMode::Deep,
        "A turn whose grounding had already been retrieved was not treated as "
        "research.");

    // A short Expert follow-up.
    RoutingInputs followUp;
    followUp.input = "Why?";
    followUp.previousDeliveredTier = IntelligenceTier::Expert;
    Check(route(followUp).selectedTier == IntelligenceTier::Expert,
        "A short follow-up to an Expert answer lost its effort.");

    // A brief social turn.
    RoutingInputs social;
    social.input = "hey";
    Check(route(social).selectedTier == IntelligenceTier::Fast,
        "A greeting did not route to the cheapest tier.");
}

// A public-audience turn is a different conversation. It must neither read this one's
// continuity nor its outcome.
void TestAPublicTurnInheritsNothingFromTheLocalThread()
{
    RoutingInputs publicTurn;
    publicTurn.input = "Why?";
    publicTurn.publicAudience = true;
    publicTurn.previousDeliveredTier = IntelligenceTier::Expert;
    publicTurn.previousTurnWasUnreliable = true;

    const RoutingContext context = BuildRoutingContext(publicTurn);
    Check(!context.previousAssistantTier.has_value(),
        "A public-audience turn inherited the local conversation's tier.");
    Check(!context.previousUncertainty,
        "A public-audience turn inherited the local conversation's last outcome.");
}

// A proactive opening is Revia choosing to speak, not a request to classify.
void TestAProactiveOpeningIsNotRouted()
{
    RoutingInputs opening;
    opening.input = "What is on my screen? Search for the latest release.";
    opening.proactive = true;
    opening.previousTurnWasUnreliable = true;
    opening.recentContextCharacters = 20000;

    const RoutingContext context = BuildRoutingContext(opening);
    Check(!context.visionRequired && !context.explicitResearch &&
        !context.previousUncertainty && context.recentContextCharacters == 0,
        "A proactive opening was classified as if it were a request.");
}

} // namespace

void RunRoutingProductionTests()
{
    TestEveryRoutingInputHasAProducer();
    TestARealRequestReachesTheExpectedTier();
    TestAPublicTurnInheritsNothingFromTheLocalThread();
    TestAProactiveOpeningIsNotRouted();
    std::cout << "Every routing input is produced by the turn that uses it, a request "
                 "with each condition reaches the tier it should, and a public or "
                 "proactive turn inherits nothing from the local thread.\n";
}
