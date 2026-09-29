#include "testSupport.h"

#include "Library/structLibrary.h"
#include "Presence/chatSelector.h"

#include <chrono>
#include <iostream>
#include <string>

// Who she answers next when several people are talking at once.
//
// A first-in queue answered whoever typed fastest. The selector scores what is waiting:
// addressed to her before overheard, the owner and moderators before viewers, a
// supporter always acknowledged, a new line before a repeat, someone who has waited
// before someone she just answered, and the other platform when it has anyone waiting.
namespace
{
using namespace std::chrono_literals;
using revia::presence::ChatSelector;
using revia::presence::ExternalAdapterEvent;
using revia::tests::Check;

using Clock = ChatSelector::Clock;

presenceSettings Settings(const float talkativeness = 0.35F)
{
    presenceSettings settings;
    settings.streamTalkativeness = talkativeness;
    settings.chatQueueCapacity = 4;
    settings.chatMaximumAgeSeconds = 90;
    settings.chatAuthorCooldownSeconds = 20;
    return settings;
}

ExternalAdapterEvent Message(const std::string& id, const std::string& author,
    const std::string& text, const bool addressed = true, const std::string& role = "viewer",
    const std::string& source = "stream")
{
    ExternalAdapterEvent event;
    event.id = id;
    event.source = source;
    event.channel = "live";
    event.authorId = author;
    event.author = author;
    event.role = role;
    event.text = text;
    event.addressedToRevia = addressed;
    return event;
}

ChatSelector Quiet(const float talkativeness = 0.35F)
{
    ChatSelector selector(Settings(talkativeness));
    selector.SetJitter(0.0);
    return selector;
}

void TestPriorityGoesToWhoAddressesHerAndWhoTheyAre()
{
    ChatSelector selector = Quiet();
    const Clock::time_point now = Clock::now();
    std::string reason;
    Check(selector.Offer(Message("a", "viewer-a", "Revia, what is a mutex?"), now, reason),
        "An addressed viewer message was refused: " + reason);
    Check(selector.Offer(Message("b", "mod-b", "settle down chat", false, "moderator"), now, reason),
        "A moderator's message was refused: " + reason);
    Check(selector.Offer(Message("c", "owner-c", "Revia, say hi to the raid", true, "owner"), now, reason),
        "The owner's message was refused: " + reason);
    Check(selector.Pending() == 3, "Three offers did not leave three waiting.");

    auto first = selector.Next(now);
    Check(first && first->id == "c", "The owner was not answered first.");
    auto second = selector.Next(now);
    Check(second && second->id == "a",
        "An addressed viewer did not outrank an unaddressed moderator.");
    auto third = selector.Next(now);
    Check(third && third->id == "b", "The moderator was not answered next.");
    Check(!selector.Next(now) && selector.Pending() == 0, "An empty queue answered someone.");
}

void TestARepeatIsNotAnsweredTwice()
{
    ChatSelector selector = Quiet();
    const Clock::time_point now = Clock::now();
    std::string reason;
    Check(selector.Offer(Message("a", "viewer-a", "Revia do a flip!"), now, reason), reason);
    Check(!selector.Offer(Message("b", "viewer-b", "revia, DO A FLIP"), now, reason) &&
        reason.find("same line") != std::string::npos,
        "The same line from someone else was queued again: " + reason);
    Check(selector.Next(now).has_value(), "The first copy was not answered.");
    Check(!selector.Offer(Message("c", "viewer-c", "Revia do a flip"), now + 1s, reason),
        "A line she just answered was queued again.");
    Check(!selector.Offer(Message("d", "viewer-d", "   "), now, reason) &&
        reason.find("no words") != std::string::npos,
        "A blank message was queued.");
}

void TestFairnessYieldsToWhoHasNotBeenAnswered()
{
    ChatSelector selector = Quiet();
    const Clock::time_point now = Clock::now();
    std::string reason;
    Check(selector.Offer(Message("a1", "loud", "Revia, first!"), now, reason), reason);
    auto first = selector.Next(now);
    Check(first && first->id == "a1", "The first message was not answered.");
    // The same person again, and someone new: the new one goes first.
    Check(selector.Offer(Message("a2", "loud", "Revia, me again!"), now + 1s, reason), reason);
    Check(selector.Offer(Message("b1", "quiet", "Revia, my turn?"), now + 2s, reason), reason);
    auto next = selector.Next(now + 3s);
    Check(next && next->id == "b1",
        "Someone she just answered was answered again ahead of someone new.");
    // After the cooldown the loud one is back on even terms.
    auto later = selector.Next(now + 30s);
    Check(later && later->id == "a2", "The cooled-down author was never answered.");

    // A supporter is acknowledged even right after being answered.
    ChatSelector paid = Quiet();
    Check(paid.Offer(Message("s1", "patron", "Revia, subbed!", true, "supporter"), now, reason), reason);
    Check(paid.Next(now) && paid.Next(now) == std::nullopt, "The supporter was not answered.");
    Check(paid.Offer(Message("s2", "patron", "Revia, gifted five", true, "supporter"), now + 1s, reason), reason);
    Check(paid.Offer(Message("v1", "viewer", "Revia, hello"), now + 1s, reason), reason);
    auto acknowledged = paid.Next(now + 2s);
    Check(acknowledged && acknowledged->id == "s2",
        "A supporter's second event was not acknowledged ahead of a viewer.");
}

void TestTwoSourcesShareHer()
{
    ChatSelector selector = Quiet();
    const Clock::time_point now = Clock::now();
    std::string reason;
    Check(selector.Offer(Message("t1", "twitch-a", "Revia, hi from twitch"), now, reason), reason);
    Check(selector.Offer(Message("d1", "discord-a", "Revia, hi from discord", true, "viewer", "discord"), now, reason), reason);
    Check(selector.Offer(Message("t2", "twitch-b", "Revia, twitch again"), now, reason), reason);
    auto first = selector.Next(now);
    Check(first && first->source == "discord",
        "A direct channel did not go first.");
    auto second = selector.Next(now);
    Check(second && second->source == "stream", "The stream never got its turn.");
}

void TestOverheardChatDependsOnTalkativeness()
{
    ChatSelector reserved = Quiet(0.2F);
    const Clock::time_point now = Clock::now();
    std::string reason;
    Check(!reserved.Offer(Message("o1", "viewer", "lol that boss", false), now, reason) &&
        reason.find("talkativeness") != std::string::npos,
        "Overheard chat was queued while she is reserved: " + reason);
    Check(reserved.RepliesPerMinute() == 4, "The reserved allowance is not four a minute.");

    ChatSelector chatty = Quiet(0.9F);
    Check(chatty.Offer(Message("o1", "viewer", "lol that boss", false), now, reason), reason);
    Check(chatty.Offer(Message("a1", "viewer-b", "Revia, what boss?"), now, reason), reason);
    auto first = chatty.Next(now);
    Check(first && first->id == "a1", "Overheard chat outranked a message addressed to her.");
    Check(chatty.RepliesPerMinute() == 11, "The chatty allowance is not eleven a minute.");
    Check(chatty.Describe(now).find("talkativeness 0.9") != std::string::npos,
        "The status line does not name the talkativeness.");
}

void TestTheAllowanceHoldsTheQueueAndAgeDropsIt()
{
    ChatSelector selector = Quiet(0.0F);
    const Clock::time_point now = Clock::now();
    std::string reason;
    for (int index = 0; index < 3; ++index)
    {
        Check(selector.Offer(Message("m" + std::to_string(index), "v" + std::to_string(index),
            "Revia, question " + std::to_string(index)), now, reason), reason);
    }
    Check(selector.Next(now) && selector.Next(now + 1s),
        "Two replies a minute were not allowed.");
    Check(!selector.Next(now + 2s) && selector.Pending() == 1,
        "A third reply in the minute was allowed at talkativeness 0.");
    Check(selector.Next(now + 61s).has_value(), "The allowance did not refresh after a minute.");

    Check(selector.Offer(Message("old", "v9", "Revia, stale?"), now + 62s, reason), reason);
    Check(!selector.Next(now + 200s) && selector.Pending() == 0,
        "A message older than the maximum age was answered.");

    // A full queue takes a newcomer only when it outscores the weakest waiting.
    ChatSelector full = Quiet(0.35F);
    for (int index = 0; index < 4; ++index)
    {
        Check(full.Offer(Message("f" + std::to_string(index), "f" + std::to_string(index),
            "Revia, filler " + std::to_string(index)), now, reason), reason);
    }
    Check(!full.Offer(Message("weak", "w", "Revia, one more"), now, reason) &&
        reason.find("full") != std::string::npos,
        "A full queue took an equal newcomer.");
    Check(full.Offer(Message("mod", "m", "Revia, mod here", true, "moderator"), now, reason) &&
        full.Pending() == 4,
        "A full queue refused a moderator who outscored the weakest waiting.");
    auto chosen = full.Next(now);
    Check(chosen && chosen->id == "mod", "The moderator did not go first once queued.");
}
} // namespace

void RunChatSelectorTests()
{
    TestPriorityGoesToWhoAddressesHerAndWhoTheyAre();
    TestARepeatIsNotAnsweredTwice();
    TestFairnessYieldsToWhoHasNotBeenAnswered();
    TestTwoSourcesShareHer();
    TestOverheardChatDependsOnTalkativeness();
    TestTheAllowanceHoldsTheQueueAndAgeDropsIt();
    std::cout << "The chat selector answers who addresses her and who they are first, never "
                 "the same line twice, yields to whoever has not been answered, shares her "
                 "between sources, and picks up overheard chat only when she is talkative.\n";
}
