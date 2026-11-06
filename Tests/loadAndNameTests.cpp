#include "testSupport.h"

#include "Identity/relationshipEvidence.h"
#include "Identity/relationshipRegistry.h"
#include "Resources/loadGovernor.h"
#include "Speech/qwenTtsClient.h"

#include <cmath>
#include <iostream>

namespace
{
using revia::tests::Check;
using namespace revia::resources;

UsageSnapshot Snapshot(const double usedFraction, const bool measured = true)
{
    UsageSnapshot snapshot;
    snapshot.measured = true;
    UsageMeter meter;
    meter.id = "gpu";
    meter.label = "GPU";
    meter.budget = 100.0;
    meter.capacity = 100.0;
    meter.used = usedFraction * 100.0;
    meter.measured = measured;
    snapshot.meters.push_back(meter);
    return snapshot;
}

// A card whose budget and physical ceiling are different numbers, which is the only
// shape in which the two can disagree -- and the shape every real GPU has.
UsageSnapshot GpuSnapshot(const double usedMiB, const double budgetMiB, const double capacityMiB)
{
    UsageSnapshot snapshot;
    snapshot.measured = true;
    UsageMeter meter;
    meter.id = "gpu:CUDA0:vram";
    meter.label = "RTX 5070 (CUDA0) VRAM";
    meter.unit = MeterUnit::Mebibytes;
    meter.budget = budgetMiB;
    meter.capacity = capacityMiB;
    meter.used = usedMiB;
    meter.measured = true;
    snapshot.meters.push_back(meter);
    return snapshot;
}

void TestLoadStatesFollowActualUsage()
{
    Check(AssessLoad(Snapshot(0.20)).state == LoadState::Free,
        "A mostly idle machine was not reported as free.");
    Check(AssessLoad(Snapshot(0.70)).state == LoadState::Normal,
        "An ordinarily busy machine was reported as something other than normal.");
    Check(AssessLoad(Snapshot(0.93)).state == LoadState::Pressured,
        "A machine near its ceiling was not reported as pressured.");
    Check(AssessLoad(Snapshot(1.05)).state == LoadState::Throttled,
        "A machine past its ceiling was not reported as throttled.");
}

void TestBudgetOverrunIsNotStarvation()
{
    // The failure this exists to prevent. Resident model weights put Revia past an
    // allowance that was carved out before they loaded, while the card itself still has
    // room to spare. Judged on the budget this reads as 110% and sheds every optional
    // thing she does; judged on the hardware it is a card that is 73% full and fine.
    // Thirteen sessions running, she throttled about thirty seconds after startup and
    // never came back, so memory consolidation and curiosity planning simply never ran.
    const UsageSnapshot overBudget = GpuSnapshot(9011.0, 8192.0, 12288.0);

    const LoadAdjustment assessed = AssessLoad(overBudget);
    Check(assessed.state == LoadState::Normal,
        "A card with room to spare was throttled for passing a budget.");
    Check(assessed.allowOptionalBackgroundWork,
        "Background work was shed because a plan was optimistic, not because a device "
        "was full.");
    Check(assessed.budgetExceeded,
        "The budget overrun was not reported at all, so the plan looks fine when it is "
        "not.");
    Check(assessed.reason.find("budget") != std::string::npos,
        "The overrun was not mentioned in the sentence the panel and the log show.");

    // The same card genuinely running out still sheds work: the fix is about which
    // number is consulted, not about never throttling.
    Check(AssessLoad(GpuSnapshot(11900.0, 8192.0, 12288.0)).state == LoadState::Throttled,
        "A card that really was nearly full was not throttled.");
}

void TestBusyIsNotFull()
{
    // Engine utilisation is a statement about how hard a card is working, not how much
    // room is left on it. Throttling on it would shed optional work at exactly the
    // moment work is happening, then restore it the instant she went idle.
    UsageSnapshot generating;
    generating.measured = true;
    UsageMeter compute;
    compute.id = "gpu:CUDA0:compute";
    compute.label = "RTX 5070 (CUDA0) compute";
    compute.unit = MeterUnit::Percent;
    compute.basis = MeterBasis::Capacity;
    compute.capacity = 100.0;
    compute.used = 100.0;
    compute.measured = true;
    generating.meters.push_back(compute);

    Check(AssessLoad(generating).state == LoadState::Normal,
        "A GPU busy doing the work it was asked to do was treated as starved.");
    Check(!AssessLoad(generating).allowOptionalBackgroundWork,
        "New background work competed with a measured busy GPU.");
}

void TestResidentGpuMemoryDoesNotStrandIdleWork()
{
    auto usage = GpuSnapshot(11400.0, 10752.0, 12288.0);
    UsageMeter compute;
    compute.id = "gpu:CUDA0:compute";
    compute.unit = MeterUnit::Percent;
    compute.capacity = 100.0;
    compute.used = 2.0;
    compute.measured = true;
    usage.meters.push_back(compute);
    const auto idle = AssessLoad(usage);
    Check(idle.state == LoadState::Pressured && idle.budgetExceeded &&
        idle.allowOptionalBackgroundWork,
        "Resident weights at 93% still permanently suppressed idle curiosity.");
    Check(!idle.allowPhraseAheadVoice && idle.voicePrefetchFragments == 2,
        "Idle background admission also expanded voice memory use.");

    usage.meters.back().used = 90.0;
    Check(!AssessLoad(usage).allowOptionalBackgroundWork &&
        !AssessLoad(usage).allowOpportunisticVision,
        "An actively computing GPU admitted competing optional work.");
    usage.meters.back().used = 2.0;
    Check(AssessLoad(usage).allowOptionalBackgroundWork,
        "Optional work could not recover without releasing the resident weights.");
    usage.meters.back().measured = false;
    Check(!AssessLoad(usage).allowOptionalBackgroundWork,
        "An unreadable high-occupancy GPU was assumed idle.");
    usage.meters.back().measured = true;
    usage.meters.front().used = 11700.0;
    Check(AssessLoad(usage).state == LoadState::Throttled &&
        AssessLoad(usage).allowOptionalBackgroundWork &&
        AssessLoad(usage).voicePrefetchFragments == 1,
        "Crossing 95% blocked idle resident inference despite sufficient free memory.");
    usage.meters.front().used = 11900.0;
    Check(!AssessLoad(usage).allowOptionalBackgroundWork,
        "Critical memory pressure was excused because compute was idle.");
    usage.meters.front().capacity = 4096.0;
    usage.meters.front().used = 3800.0;
    Check(!AssessLoad(usage).allowOptionalBackgroundWork,
        "A smaller card admitted work without sufficient absolute headroom.");
    usage.meters.front().capacity = 12288.0;
    usage.meters.front().used = 11400.0;
    auto otherGpu = compute;
    otherGpu.id = "gpu:CUDA1:compute";
    otherGpu.used = 85.0;
    usage.meters.push_back(otherGpu);
    Check(!AssessLoad(usage).allowOptionalBackgroundWork,
        "The second GPU's active load was ignored.");
    usage.meters.pop_back();
    auto ram = usage.meters.front();
    ram.id = "ram";
    usage.meters.push_back(ram);
    Check(!AssessLoad(usage).allowOptionalBackgroundWork,
        "GPU residency excused simultaneous RAM pressure.");
    usage.meters.back().id = "cpu";
    usage.meters.back().unit = MeterUnit::Threads;
    Check(!AssessLoad(usage).allowOptionalBackgroundWork,
        "GPU residency excused simultaneous CPU pressure.");
}

void TestOptionalWorkShedsBeforeConversationQuality()
{
    // The ordering that matters: background work stops first, then phrase-ahead voice.
    // A person will not miss curiosity planning; they will notice a stuttering voice.
    const LoadAdjustment pressured = AssessLoad(Snapshot(0.93));
    Check(!pressured.allowOptionalBackgroundWork,
        "Background work kept running while the machine was pressured.");
    Check(pressured.allowOpportunisticVision,
        "An extra vision round trip was refused before background work was.");

    const LoadAdjustment throttled = AssessLoad(Snapshot(1.05));
    Check(!throttled.allowOptionalBackgroundWork && !throttled.allowPhraseAheadVoice &&
        !throttled.allowOpportunisticVision,
        "Something optional was still permitted while a resource was being starved.");
    Check(throttled.voicePrefetchFragments >= 1,
        "Prefetch was reduced to nothing, which would stall playback entirely.");

    // Spare capacity is spent on the one thing that shortens the gap between sentences.
    const LoadAdjustment free = AssessLoad(Snapshot(0.20));
    Check(free.voicePrefetchFragments > pressured.voicePrefetchFragments,
        "Spare capacity did not buy any additional prefetch.");
    Check(free.allowPhraseAheadVoice, "A free machine refused to work ahead.");
}

void TestUnmeasurableLoadChangesNothing()
{
    // Guessing is the failure here. Assuming idle invites work the machine cannot carry;
    // assuming busy leaves an unmeasurable platform permanently degraded.
    UsageSnapshot nothing;
    nothing.measured = false;
    const LoadAdjustment blind = AssessLoad(nothing);
    Check(blind.state == LoadState::Normal,
        "An unmeasurable machine was assumed to be in some particular state.");
    Check(blind.allowOptionalBackgroundWork && blind.allowPhraseAheadVoice,
        "An unmeasurable machine was degraded on no evidence.");

    // A meter that cannot be read is ignored rather than counted as idle.
    Check(AssessLoad(Snapshot(1.20, false)).state == LoadState::Normal,
        "An unmeasured meter was treated as a real reading.");
}

void TestBatchClipFramingRefusesAnythingAmbiguous()
{
    using revia::speech::ParseBatchClipSizes;

    // The one shape that may be used: as many lengths as phrases, accounting for every
    // byte. Order is the whole contract -- these lengths are what map clip 2 onto the
    // second phrase rather than the third.
    const auto good = ParseBatchClipSizes("10,20,30", 60, 3);
    Check(good.has_value(), "A well formed clip framing was rejected.");
    Check(good->size() == 3 && (*good)[0] == 10 && (*good)[1] == 20 && (*good)[2] == 30,
        "A well formed clip framing was parsed into the wrong lengths.");

    // Every one of these would produce audio attached to the wrong phrase, or a silent
    // slot ordered playback would wait behind. None may be repaired into a usable split.
    Check(!ParseBatchClipSizes("10,20", 60, 3).has_value(),
        "A framing naming fewer clips than phrases was accepted.");
    Check(!ParseBatchClipSizes("10,20,30,40", 100, 3).has_value(),
        "A framing naming more clips than phrases was accepted.");
    Check(!ParseBatchClipSizes("10,20,30", 61, 3).has_value(),
        "A framing that did not account for the whole payload was accepted.");
    Check(!ParseBatchClipSizes("10,20,29", 60, 3).has_value(),
        "Lengths that did not sum to the payload were accepted.");
    Check(!ParseBatchClipSizes("10,,30", 40, 3).has_value(),
        "A doubled comma was accepted, which would shift every later clip.");
    Check(!ParseBatchClipSizes("10,20,", 30, 3).has_value(),
        "A trailing comma was accepted.");
    Check(!ParseBatchClipSizes("10,20,0", 30, 3).has_value(),
        "A zero-length clip was accepted, which never becomes audible.");
    Check(!ParseBatchClipSizes("10,x,30", 60, 3).has_value(),
        "A non-numeric length was accepted.");
    Check(!ParseBatchClipSizes("", 0, 3).has_value(),
        "An empty framing header was accepted.");
    Check(!ParseBatchClipSizes("10", 10, 0).has_value(),
        "A framing was accepted for a batch of no phrases.");
    // 2^64 - 10 and 70 wrap to 60. Accepted, the second clip would start past the end
    // of a 60-byte body.
    Check(!ParseBatchClipSizes("18446744073709551606,70", 60, 2).has_value(),
        "Lengths whose sum wraps around to the payload size were accepted.");

    // A single-clip batch is still framed, because the per-phrase path and the batch
    // path publish through the same code and one of them must not be a special case.
    const auto single = ParseBatchClipSizes("42", 42, 1);
    Check(single.has_value() && single->size() == 1 && (*single)[0] == 42,
        "A single-clip batch framing was rejected.");
}

void TestNamesAreReadOnlyFromRealIntroductions()
{
    using namespace revia::identity;

    Check(ReadStatedName("my name is Quentin") == "Quentin",
        "A plain introduction was not recognised.");
    Check(ReadStatedName("you can call me Quentin, by the way") == "Quentin",
        "An introduction with trailing words was not recognised.");
    Check(ReadStatedName("hey, call me Sensei") == "Sensei",
        "A short-form introduction was not recognised.");

    // Only one word. Taking the rest of the sentence would store a clause as a name.
    Check(ReadStatedName("my name is Quentin and I work on Revia") == "Quentin",
        "A name absorbed the rest of the sentence.");

    // The cases that would otherwise rename someone to a stray word.
    Check(ReadStatedName("call me later").empty(),
        "\"call me later\" was stored as somebody's name.");
    Check(ReadStatedName("this is fine").empty(),
        "\"this is fine\" was stored as somebody's name.");
    Check(ReadStatedName("what is your name?").empty(),
        "Asking her name was mistaken for stating one.");
    Check(ReadStatedName("I think the name is wrong").empty(),
        "A passing mention of a name was treated as an introduction.");

    // "this is" is an introduction only where someone would introduce themselves.
    // Each of these renamed the local user in a real session.
    Check(ReadStatedName("this is where the build breaks").empty(),
        "\"this is where\" was stored as somebody's name.");
    Check(ReadStatedName("I think this is not right").empty(),
        "\"this is not\" was stored as somebody's name.");
    Check(ReadStatedName("ok so this is part of the renderer").empty(),
        "\"this is part\" was stored as somebody's name.");
    Check(ReadStatedName("Look, this is Quentin's code").empty(),
        "A mid-sentence \"this is\" was treated as an introduction.");
    Check(ReadStatedName("Hi Revia, this is Quentin") == "Quentin",
        "An introduction after a greeting was not recognised.");
    Check(ReadStatedName("this is Sensei.") == "Sensei",
        "An introduction at the start was not recognised.");
    Check(ReadStatedName("can you recall me Tuesday").empty(),
        "\"call me\" matched inside another word.");
}

// Naming oneself versus someone arriving; both withhold what an unnamed speaker said, and
// differ only in what she is told.
void TestNamingOneselfIsNotAnArrival()
{
    using revia::identity::NamesThemselves;
    using revia::tests::Check;
    Check(NamesThemselves("btw call me Mari") && NamesThemselves("my name is Noor") && NamesThemselves("name's Theo btw") &&
              NamesThemselves("you can call me Lee"),
        "Naming oneself was not recognised.");
    Check(!NamesThemselves("hi, this is Sam!") && !NamesThemselves("hey, it's Mari") && !NamesThemselves("Sam here") &&
              !NamesThemselves("call me later"),
        "An arrival or an ordinary remark was read as someone naming themselves.");
}

// An explicit handover is a different person even when the newcomer says "my name is".
void TestAHandoverIsAnotherPerson()
{
    using revia::identity::AnnouncesAnotherPerson;
    using revia::tests::Check;
    for (const char* handover : {"Noor left. My name is Sam", "I'm a different person. Call me Sam.", "noor's gone, my name is Sam",
             "she had to go, you can call me Sam", "hey, someone else here now. call me Sam", "not the same person btw, name's Sam",
             "my turn! I'm called Sam", "Noor just stepped away. my name is Sam", "Noor logged off, call me Sam",
             "ok Noor went home. my name is Sam", "Sam taking over from Noor, my name is Sam"})
        Check(AnnouncesAnotherPerson(handover), std::string("A handover was not read as another person: ") + handover);
    for (const char* same : {"btw call me Mari", "my name is Noor", "I left my keys at work, call me Mari", "turn left at the light",
             "there's nothing left, my name is Mari", "I'm not a different person, call me Mari", "the pain is finally better, call me Mari",
             "She said \"I'm a different person\" lol. call me Mari", "I had to go, call me Mari"})
        Check(!AnnouncesAnotherPerson(same), std::string("An ordinary remark was read as a handover: ") + same);
}

void TestSpeakerNameCorrectionsRejectTheNameInUse()
{
    using namespace revia::identity;

    Check(ReadSpeakerNameCorrection("sorry, I meant Mari, not Jo. Jo is my cousin lol", "Jo") == "Mari",
        "A self-correction away from the name in use was not read.");
    Check(ReadSpeakerNameCorrection("no no, it's Mari not Jo", "jo") == "Mari", "\"it's X not Y\" was not read.");
    Check(ReadSpeakerNameCorrection("I'm Mari, not Jo!", "Jo") == "Mari", "\"I'm X, not Y\" was not read.");
    Check(ReadSpeakerNameCorrection("I\xE2\x80\x99m Mari, not Jo", "Jo") == "Mari", "A curly apostrophe hid the correction.");
    Check(ReadSpeakerNameCorrection("I said Mari, not Jo", "Jo") == "Mari", "\"I said X, not Y\" was not read.");

    // Only the name currently in use can be rejected.
    Check(ReadSpeakerNameCorrection("I meant Mari, not Jo", "Sam").empty(), "A name not in use was treated as rejected.");
    Check(ReadSpeakerNameCorrection("I meant Mari, not Jo", "").empty(), "An unnamed speaker was given a correction.");
    // Contrasts about other people, quoted words and stray words are not self-corrections.
    Check(ReadSpeakerNameCorrection("send the invite to Ana, not Jo", "Jo").empty(), "Naming a recipient renamed the speaker.");
    Check(ReadSpeakerNameCorrection("I told Ana, not Jo", "Jo").empty(), "Telling someone renamed the speaker.");
    Check(ReadSpeakerNameCorrection("Ana, not Jo, is coming", "Jo").empty(), "A third person renamed the speaker.");
    Check(ReadSpeakerNameCorrection("She wrote: \"I meant Ana, not Jo\"", "Jo").empty(), "A quotation renamed the speaker.");
    Check(ReadSpeakerNameCorrection("I meant later, not Jo", "Jo").empty(), "A stray word was read as a name.");
    Check(
        ReadSpeakerNameCorrection("I meant Mari. Not Jo though, he's fine", "Jo").empty(), "A correction was assembled across sentences.");
}

void TestReturningSpeakersAreOnlyKnownPeople()
{
    using namespace revia::identity;

    const std::vector<std::string> known = {"Mari", "Jo"};
    Check(ReadReturningSpeaker("hey, it's Mari", known) == "Mari", "A returning greeting was not read.");
    Check(ReadReturningSpeaker("I'm mari again!", known) == "Mari", "\"I'm X again\" was not read.");
    Check(ReadReturningSpeaker("Mari here", known) == "Mari", "\"X here\" was not read.");
    Check(ReadReturningSpeaker("hi revia, it's me, Jo", known) == "Jo", "\"it's me, X\" was not read.");
    Check(ReadReturningSpeaker("It\xE2\x80\x99s Mari", known) == "Mari", "A curly apostrophe hid a returning greeting.");

    // Ordinary remarks, unknown names, possessives and later clauses name nobody.
    Check(ReadReturningSpeaker("hey, it's late", known).empty(), "An ordinary remark named a person.");
    Check(ReadReturningSpeaker("I'm tired", known).empty(), "A feeling named a person.");
    Check(ReadReturningSpeaker("it's Zed", known).empty(), "An unknown name was adopted as a returning person.");
    Check(ReadReturningSpeaker("I'm Mari's cousin", known).empty(), "Mentioning a known person named them.");
    Check(ReadReturningSpeaker("the bug is fixed, it's Mari", known).empty(), "A later clause was read as an introduction.");
    Check(ReadReturningSpeaker("She said \"it's Mari\"", known).empty(), "A quotation named a person.");
    Check(ReadReturningSpeaker("hey, it's Mari", {}).empty(), "Nobody known still produced a returning person.");
}

void TestLearningANameKeepsEverythingEarned()
{
    using namespace revia::identity;
    revia::tests::ScopedTestDirectory directory;
    RelationshipRegistry registry(directory.root / "identity.json");
    std::string error;
    Check(registry.Load(error), error);

    const std::string entity = LocalUserEntityId();
    for (int turn = 0; turn < 80; ++turn)
    {
        registry.Apply(BuildRelationshipEvent(entity,
            ReadConversationSignals("thanks, that genuinely helped", "sure", true)));
    }
    const RelationshipState before = registry.Get(entity);
    Check(before.interactionCount == 80, "The setup did not accumulate history.");

    registry.SetDisplayName(entity, "Quentin");
    const RelationshipState after = registry.Get(entity);

    // The entity id is unchanged, which is the whole point: learning someone's name is
    // not meeting a stranger, and re-keying would discard everything earned before it.
    Check(after.entityId == before.entityId, "Learning a name re-keyed the relationship.");
    Check(after.interactionCount == before.interactionCount,
        "Learning a name reset the interaction history.");
    Check(after.familiarity == before.familiarity && after.trust == before.trust,
        "Learning a name discarded earned familiarity or trust.");
    Check(after.displayName == "Quentin", "The learned name was not stored.");
}
}

void TestSeveralPeopleAtOneKeyboard()
{
    using namespace revia::identity;
    revia::tests::ScopedTestDirectory directory;
    RelationshipRegistry registry(directory.root / "identity.json");
    std::string error;
    Check(registry.Load(error), error);

    // History accumulated before anyone introduced themselves.
    for (int turn = 0; turn < 40; ++turn)
    {
        registry.Apply(BuildRelationshipEvent(LocalUserEntityId(),
            ReadConversationSignals("thanks, that helped a lot", "sure", true)));
    }

    // The first explicit introduction adopts the anonymous local history.
    const std::string first = registry.ResolveNamedLocalSpeaker("Quentin");
    Check(first != LocalUserEntityId(), "A named speaker kept the anonymous id.");
    const RelationshipState quentin = registry.Get(first);
    Check(quentin.interactionCount == 40,
        "The first person to introduce themselves did not inherit the history.");
    Check(quentin.displayName == "Quentin", "The name was not stored.");
    Check(!registry.Find(LocalUserEntityId()).has_value(),
        "The anonymous entity lingered after being adopted.");

    // A second person at the same keyboard is their own relationship, starting neutral.
    const std::string second = registry.ResolveNamedLocalSpeaker("Sam");
    Check(second != first, "Two people at one keyboard collapsed into one relationship.");
    const RelationshipState sam = registry.Get(second);
    Check(sam.interactionCount == 0 && std::abs(sam.affinity) < 0.0001F,
        "A second person inherited trust earned by somebody else.");
    Check(registry.Get(first).interactionCount == 40,
        "Introducing a second person disturbed the first one's history.");

    // Coming back is recognised rather than starting again.
    Check(registry.ResolveNamedLocalSpeaker("quentin") == first,
        "A returning person was not recognised across capitalisation.");
    Check(registry.Count() == 2, "Returning created a duplicate relationship.");
}

void TestExplicitIdentitySurvivesWithoutDefaultGuess()
{
    using namespace revia::identity;
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    std::string error;
    {
        RelationshipRegistry registry(path);
        Check(registry.Load(error), error);
        Check(registry.DefaultLocalSpeaker() == LocalUserEntityId(), "Nobody has been named yet, so the speaker should be anonymous.");
        const std::string quentin = registry.ResolveNamedLocalSpeaker("Quentin");
        registry.Apply(BuildRelationshipEvent(quentin, ReadConversationSignals("thanks, that helped a lot", "sure", true)));
        Check(registry.Save(error), error);
        Check(registry.DefaultLocalSpeaker() == LocalUserEntityId(), "An unknown speaker was guessed from the one saved person.");
        Check(registry.Get(quentin).interactionCount == 1, "The explicitly identified person lost their interaction history.");
    }
    RelationshipRegistry restarted(path);
    Check(restarted.Load(error), error);
    Check(restarted.DefaultLocalSpeaker() == LocalUserEntityId(), "A restart attributed an unknown speaker to the one saved person.");
    const auto quentin = restarted.Find(RelationshipRegistry::NamedLocalEntityId("Quentin"));
    Check(quentin && quentin->displayName == "Quentin" && quentin->interactionCount == 1,
        "A restart discarded the named person's identity or history.");
    Check(restarted.ResolveNamedLocalSpeaker("quentin") == quentin->entityId && restarted.Count() == 1,
        "An explicit returning introduction did not restore the existing person.");
    Check(restarted.Get(restarted.DefaultLocalSpeaker()).interactionCount == 0 && restarted.Get(quentin->entityId).interactionCount == 1,
        "An unknown speaker inherited the saved person's history.");

    (void)restarted.ResolveNamedLocalSpeaker("Sam");
    Check(restarted.DefaultLocalSpeaker() == LocalUserEntityId(), "The speaker was guessed between two people who share the keyboard.");
}

void RunLoadAndNameTests()
{
    TestLoadStatesFollowActualUsage();
    TestBudgetOverrunIsNotStarvation();
    TestBusyIsNotFull();
    TestResidentGpuMemoryDoesNotStrandIdleWork();
    TestOptionalWorkShedsBeforeConversationQuality();
    TestUnmeasurableLoadChangesNothing();
    TestBatchClipFramingRefusesAnythingAmbiguous();
    TestNamesAreReadOnlyFromRealIntroductions();
    TestNamingOneselfIsNotAnArrival();
    TestAHandoverIsAnotherPerson();
    TestSpeakerNameCorrectionsRejectTheNameInUse();
    TestReturningSpeakersAreOnlyKnownPeople();
    TestLearningANameKeepsEverythingEarned();
    TestSeveralPeopleAtOneKeyboard();
    TestExplicitIdentitySurvivesWithoutDefaultGuess();
    std::cout << "Load sheds optional work on how full a device is rather than on a "
                 "budget, batched voice clips are refused unless their framing is\n"
                 "exact, and a learned name keeps everything already earned.\n";
}
