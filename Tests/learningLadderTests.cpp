#include "reviaSessionTestAccess.h"
#include "conversationRuntimeTestAccess.h"
#include "Learning/playbook.h"
#include "Learning/skillLibrary.h"

#include <iostream>

namespace
{
using revia::learning::Playbook;
using revia::learning::PlaybookEntry;
using revia::learning::Procedure;
using revia::learning::SkillLibrary;
using revia::runtime::ReviaSession;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

revia::goals::Goal FinishedGoal(const std::string& title, const revia::goals::GoalStatus status, const int steps)
{
    revia::goals::Goal goal;
    goal.id = revia::goals::NewGoalId();
    goal.title = title;
    goal.status = status;
    for (int index = 0; index < steps; ++index)
    {
        revia::goals::GoalStep step;
        step.ordinal = static_cast<std::uint32_t>(index);
        step.description = index == 0 ? "Create the folder" : "Move the file into it";
        step.action.type = index == 0 ? revia::actions::ActionType::CreateDirectory : revia::actions::ActionType::MoveFile;
        step.action.source = index == 0 ? "C:/Users/me/Documents/Receipts" : "C:/Users/me/Downloads/receipt.pdf";
        goal.steps.push_back(step);
    }
    return goal;
}

void TestThePlaybookHoldsGuidanceForEveryoneAndForOnePerson()
{
    revia::tests::ScopedTestDirectory directory;
    const auto file = directory.root / "playbook.json";
    Playbook book;
    std::string error;
    Check(book.Initialize(file, error), "A new playbook could not start.");
    Check(!book.Add("hi", "", "", "owner", "", true, error).has_value() && Contains(error, "few words"),
        "A two-letter line was accepted.");
    const auto everyone = book.Add("Keep answers short after 10pm.", "", "", "owner", "", true, error);
    const auto sam = book.Add("Give the reasoning before the answer.", "person:sam", "Sam", "owner", "", true, error);
    const auto lesson = book.Add("Plans with fewer than four steps finish more often.", "", "", "lesson",
        "12 of 14 short plans finished", false, error);
    Check(everyone && sam && lesson && book.Size() == 3, "Lines were not added.");
    Check(book.Add("keep answers SHORT after 10pm", "", "", "owner", "", true, error)->id == everyone->id && book.Size() == 3,
        "The same line in other letters was added twice.");
    Check(book.Add("Keep answers short after 10pm.", "person:sam", "Sam", "owner", "", true, error)->id != everyone->id,
        "The same words for a different person were treated as the same line.");

    const std::string forSam = book.Render("person:sam");
    Check(Contains(forSam, "# Playbook") && Contains(forSam, "Keep answers short") &&
            Contains(forSam, "reasoning before the answer. (about Sam)") && !Contains(forSam, "fewer than four"),
        "The block for Sam is wrong:\n" + forSam);
    const std::string forOther = book.Render("person:other");
    Check(Contains(forOther, "Keep answers short") && !Contains(forOther, "reasoning before"),
        "A line about Sam reached someone else.");
    Check(book.Render("person:other", 0).empty(), "A zero budget rendered lines.");
    Check(book.SetEnabled(3, true) && Contains(book.Render(""), "fewer than four"), "Turning a line on did not show it.");
    Check(book.SetEnabled(1, false) && !Contains(book.Render(""), "Keep answers short"), "Turning a line off did not hide it.");
    Check(!book.SetEnabled(9, true), "A missing number was accepted.");

    Playbook reopened;
    Check(reopened.Initialize(file, error), "The playbook could not be reopened.");
    const std::vector<PlaybookEntry> entries = reopened.Entries();
    Check(entries.size() == 4 && !entries[0].enabled && entries[1].scope == "person:sam" &&
            entries[2].source == "lesson" && entries[2].enabled && entries[0].uses == 3,
        "The playbook did not survive a restart with its state.");
    Check(reopened.Remove(2).has_value() && reopened.Size() == 3 && !reopened.Remove(7).has_value(),
        "Removing a line did not work as numbered.");
    Check(Contains(Playbook::Normalized("  Keep, it SHORT!  "), "keep it short"), "Normalization is wrong.");
}

void TestProceduresAreKeptFromWhatWorkedAndOfferedForWhatResembles()
{
    revia::tests::ScopedTestDirectory directory;
    const auto file = directory.root / "procedures.json";
    SkillLibrary library;
    std::string error;
    Check(library.Initialize(file, error), "A new library could not start.");
    Check(!library.Learn(FinishedGoal("Tidy the receipts", revia::goals::GoalStatus::Failed, 2)).has_value(),
        "A failed goal was kept as a procedure.");
    Check(!library.Learn(FinishedGoal("Nothing to do", revia::goals::GoalStatus::Succeeded, 0)).has_value(),
        "A goal with no steps was kept.");
    const auto kept = library.Learn(FinishedGoal("Move the receipt from Downloads into a Receipts folder",
        revia::goals::GoalStatus::Succeeded, 2));
    Check(kept && kept->steps.size() == 2 && Contains(kept->steps[0], "Create the folder (create_directory Receipts)") &&
            Contains(kept->steps[1], "move_file receipt.pdf") && kept->successes == 1,
        "The procedure was not kept with its steps: " + (kept ? kept->steps[0] : std::string("none")));
    const auto again = library.Learn(FinishedGoal("move the receipt from downloads into a receipts folder",
        revia::goals::GoalStatus::Succeeded, 2));
    Check(again && again->id == kept->id && again->successes == 2 && library.Entries().size() == 1,
        "Doing the same thing again did not count as another success.");

    Check(SkillLibrary::Similarity("move the receipt into the receipts folder", "move receipt to receipts folder") > 0.6,
        "Similar requests scored low.");
    Check(SkillLibrary::Similarity("move the receipt into the receipts folder", "play some music") < 0.1,
        "Different requests scored high.");
    const std::vector<Procedure> offered = library.Similar("put the new receipt into my Receipts folder");
    Check(offered.size() == 1 && offered.front().id == kept->id, "A similar request found nothing.");
    Check(library.Similar("start a timer for pasta").empty(), "An unrelated request was offered a procedure.");
    const std::string hints = SkillLibrary::RenderForPlanner(offered);
    Check(Contains(hints, "own record") && Contains(hints, "worked 2 times") && Contains(hints, "- Create the folder"),
        "The planner block is wrong:\n" + hints);
    Check(SkillLibrary::RenderForPlanner({}).empty(), "An empty offer rendered text.");

    library.RecordFailure("move a receipt into the receipts folder");
    library.RecordFailure("move a receipt into the receipts folder");
    Check(library.Similar("put the new receipt into my Receipts folder").empty(),
        "A procedure that failed as often as it worked was still offered.");
    SkillLibrary reopened;
    Check(reopened.Initialize(file, error) && reopened.Entries().size() == 1 && reopened.Entries().front().failures == 2,
        "The library did not survive a restart.");
    Check(reopened.Forget(1).has_value() && reopened.Entries().empty(), "Forgetting did not work.");
}

void TestTheSessionCarriesTheLadder()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    std::string error;
    Check(Access::Playbook(session).Initialize(directory.root / "playbook.json", error) &&
            Access::Procedures(session).Initialize(directory.root / "procedures.json", error),
        "The session's books could not start.");
    revia::runtime::ConversationRuntime& runtime = Access::Conversation(session);
    runtime.SetPlaybookProvider([&session](const std::string& speaker) { return Access::Playbook(session).Render(speaker); });

    const auto empty = Access::SubmitOperator(session, "/playbook");
    Check(empty.succeeded && Contains(empty.text, "playbook is empty"), "An empty playbook was not described: " + empty.text);
    const auto added = Access::SubmitOperator(session, "/playbook add Keep answers short after 10pm");
    Check(added.succeeded && Contains(added.text, "Noted: Keep answers short"), "A line was not added: " + added.text);
    Check(!Access::SubmitOperator(session, "/playbook add for Nobody: be brief").succeeded,
        "A line for someone she does not know was accepted.");
    Access::RememberIdentity(session, "Sam");
    const auto forSam = Access::SubmitOperator(session, "/playbook add for Sam: give the reasoning first");
    Check(forSam.succeeded && Contains(forSam.text, "Noted for Sam"), "A line for a known person was refused: " + forSam.text);
    const std::string block = revia::runtime::ConversationRuntimeTestAccess::PlaybookBlock(
        runtime, revia::identity::LocalUserEntityId());
    Check(Contains(block, "Keep answers short") && Contains(block, "reasoning first (about Sam)"),
        "The prompt block does not carry the lines for the local user:\n" + block);
    Check(!Contains(revia::runtime::ConversationRuntimeTestAccess::PlaybookBlock(runtime, "person:stranger"), "reasoning first"),
        "A stranger got Sam's line.");
    const auto listed = Access::SubmitOperator(session, "/playbook");
    Check(Contains(listed.text, "1. Keep answers short") && Contains(listed.text, "2. give the reasoning first (about Sam)") &&
            Contains(listed.text, "used 1 time"),
        "The list is wrong: " + listed.text);
    Check(Access::SubmitOperator(session, "/playbook off 1").succeeded &&
            !Contains(revia::runtime::ConversationRuntimeTestAccess::PlaybookBlock(runtime, ""), "Keep answers short"),
        "Turning a line off through the command did not take.");

    Check(Contains(Access::SubmitOperator(session, "/procedures").text, "No procedures yet"), "An empty library was not described.");
    Access::FinishGoal(session, FinishedGoal("Move the receipt from Downloads into a Receipts folder",
        revia::goals::GoalStatus::Succeeded, 2));
    const auto procedures = Access::SubmitOperator(session, "/procedures");
    Check(Contains(procedures.text, "1. Move the receipt from Downloads") && Contains(procedures.text, "2 steps, worked 1 time"),
        "A finished goal did not become a procedure: " + procedures.text);
    Check(Contains(Access::ProcedureHints(session, "move this receipt into the receipts folder"), "own record"),
        "A similar request got no hints.");
    Check(Contains(Access::SubmitOperator(session, "/procedures for move the receipt to the receipts folder").text, "worked 1 time"),
        "/procedures for did not show the offer.");
    Access::FinishGoal(session, FinishedGoal("move the receipt into the receipts folder", revia::goals::GoalStatus::Failed, 1));
    Check(Contains(Access::SubmitOperator(session, "/procedures").text, "failed 1") &&
            Contains(Access::SubmitOperator(session, "/procedures").text, "not offered"),
        "A failure was not recorded against the procedure.");
    Check(Contains(Access::SubmitOperator(session, "/procedures forget 1").text, "Forgotten"), "Forgetting failed.");
}

} // namespace

void RunLearningLadderTests()
{
    TestThePlaybookHoldsGuidanceForEveryoneAndForOnePerson();
    TestProceduresAreKeptFromWhatWorkedAndOfferedForWhatResembles();
    TestTheSessionCarriesTheLadder();
    std::cout << "The playbook says what works and reaches the turns it is for; procedures that "
        "worked are kept and offered to the planner for what resembles them.\n";
}
