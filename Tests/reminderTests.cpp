#include "reviaSessionTestAccess.h"
#include "Planning/reminders.h"

#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

namespace
{
using namespace std::chrono_literals;
using revia::planning::ParseReminderRequest;
using revia::planning::ReminderBook;
using revia::planning::ReminderParse;
using revia::planning::ReminderRequest;
using revia::planning::WallClock;
using revia::runtime::ReviaSession;
using revia::runtime::RuntimeEvent;
using revia::runtime::RuntimeEventKind;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// 10:00 on a June morning, local time: far from any daylight-saving change, so "at 3pm"
// is exactly five hours away in every time zone.
WallClock::time_point JuneMorning()
{
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 5;
    local.tm_mday = 10;
    local.tm_hour = 10;
    local.tm_isdst = -1;
    return WallClock::from_time_t(std::mktime(&local));
}

ReminderRequest Parsed(const std::string& input, const WallClock::time_point now)
{
    ReminderRequest request;
    std::string error;
    const ReminderParse parse = ParseReminderRequest(input, now, request, error);
    Check(parse == ReminderParse::Parsed, "Not read as a reminder: " + input + " " + error);
    return request;
}

void ExpectIn(const std::string& input, const WallClock::duration expected,
    const std::string& text, const WallClock::time_point now = JuneMorning())
{
    const ReminderRequest request = Parsed(input, now);
    Check(request.due - now == expected,
        "Wrong time for: " + input + " (" + revia::planning::DescribeSpan(request.due - now) + ")");
    Check(request.text == text, "Wrong reminder text for: " + input + " -> '" + request.text + "'");
}

void TestRemindersAreReadWithoutTheModel()
{
    ExpectIn("remind me in 10 minutes to stretch", 10min, "stretch");
    ExpectIn("Revia, remind me to take my pills in 2 hours.", 2h, "take your pills");
    ExpectIn("remind me in an hour and a half to check the oven", 90min, "check the oven");
    ExpectIn("can you remind me in half an hour to call Sam?", 30min, "call Sam");
    ExpectIn("/remind 1h30m stand up", 90min, "stand up");
    ExpectIn("remind me at 3pm to call Sam", 5h, "call Sam");
    ExpectIn("remind me about the standup at noon", 2h, "the standup");
    ExpectIn("remind me at 9 to lock up", 11h, "lock up");
    ExpectIn("remind me tomorrow at 9 to email Jo", 23h, "email Jo");
    ExpectIn("remind me to water the plants tomorrow at 3", 29h, "water the plants");
    ExpectIn("remind me tonight at 8 to call home", 10h, "call home");
    ExpectIn("remind me to leave at 15:30", 5h + 30min, "leave");
    ExpectIn("remind me that I am meeting Ana at 7:45 pm", 9h + 45min, "you are meeting Ana");
    ExpectIn("Revia remind me to call the bank at five thirty pm please", 7h + 30min, "call the bank");
    ExpectIn("remind me at seven forty five to leave", 9h + 45min, "leave");

    const ReminderRequest timer = Parsed("set a timer for 5 minutes", JuneMorning());
    Check(timer.timer && timer.timerLength == "5 minutes" && timer.text.empty(),
        "A timer was not read as one.");
    Check(revia::planning::Announcement(timer) == "Your timer for 5 minutes is done.",
        "A timer announces itself wrongly: " + revia::planning::Announcement(timer));
    ExpectIn("set a 10 minute timer for the pasta", 10min, "the pasta");
    ExpectIn("remind me in twenty five minutes", 25min, "");

    for (const char* ordinary : {"remind me what we talked about yesterday",
             "remind me about our trip", "what time is it", "an hour ago I set a timer",
             "set a timer for 5 minutes and then call me", "10 minutes is plenty", ""})
    {
        ReminderRequest ignored;
        std::string error;
        Check(ParseReminderRequest(ordinary, JuneMorning(), ignored, error) ==
                ReminderParse::NotAReminder,
            std::string("Ordinary speech was taken as a reminder: ") + ordinary);
    }
    for (const char* unclear : {"remind me to call mom", "remind me in a bit to stretch",
             "remind me in 45 days to renew", "remind me at 3pm", "/remind",
             "remind me today at 8am to leave"})
    {
        ReminderRequest ignored;
        std::string error;
        Check(ParseReminderRequest(unclear, JuneMorning(), ignored, error) ==
                ReminderParse::Invalid && !error.empty(),
            std::string("A reminder she cannot set was not questioned: ") + unclear);
    }
    Check(revia::planning::DescribeSpan(40s) == "40 s" &&
            revia::planning::DescribeSpan(125min) == "2 h 5 min",
        "Spans are described wrongly.");
}

void TestTheBookKeepsThemAcrossRestarts()
{
    revia::tests::ScopedTestDirectory directory;
    const auto file = directory.root / "Reminders" / "reminders.json";
    const WallClock::time_point now = JuneMorning();
    std::string error;
    ReminderBook book;
    Check(book.Initialize(file, error), "A new reminder book could not start.");
    for (const auto& [text, offset] : std::vector<std::pair<std::string, WallClock::duration>>{
             {"later", 2h}, {"soonest", 5min}, {"middle", 1h}})
    {
        ReminderRequest request;
        request.text = text;
        request.due = now + offset;
        Check(book.Add(request, error).has_value() && error.empty(), "A reminder was not saved.");
    }

    ReminderBook reopened;
    Check(reopened.Initialize(file, error) && reopened.Pending().size() == 3 &&
            reopened.Pending().front().request.text == "soonest",
        "Reminders did not survive a restart in order.");
    const auto due = reopened.TakeDue(now + 90min);
    Check(due.size() == 2 && due[0].request.text == "soonest" && due[1].request.text == "middle",
        "Due reminders were not delivered earliest first.");
    Check(reopened.Cancel(1).has_value() && !reopened.Cancel(9).has_value() &&
            reopened.Pending().empty(),
        "Cancelling by number did not work.");

    ReminderBook afterDelivery;
    Check(afterDelivery.Initialize(file, error) && afterDelivery.Pending().empty(),
        "Delivered or cancelled reminders came back after a restart.");

    std::ofstream(file) << "{ not json";
    ReminderBook damaged;
    Check(!damaged.Initialize(file, error) && damaged.Pending().empty() &&
            std::filesystem::exists(file.string() + ".unreadable"),
        "A damaged reminders file was not set aside.");

    ReminderRequest many;
    many.text = "again";
    many.due = now + 1h;
    for (std::size_t index = 0; index < ReminderBook::MaximumPending; ++index)
    {
        (void)damaged.Add(many, error);
    }
    error.clear();
    Check(!damaged.Add(many, error).has_value() && !error.empty(),
        "The reminder book grew without limit.");
}

void TestSheSetsAndDeliversThem()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    std::mutex mutex;
    std::vector<std::string> said;
    const auto id = session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        if (event.kind != RuntimeEventKind::AssistantMessage || event.component != "Reminder") return;
        std::lock_guard lock(mutex);
        said.push_back(event.message);
    });

    const auto now = WallClock::now();
    const auto set = Access::SubmitOperator(session, "Revia, remind me in 10 minutes to stretch.");
    Check(set.fromAssistant && Contains(set.text, "stretch") && Contains(set.text, "10 min"),
        "She did not confirm the reminder: " + set.text);
    Check(Contains(Access::SubmitOperator(session, "/reminders").text, "1. stretch"),
        "The reminder is not listed.");
    Check(Contains(Access::Reminders(session), "stretch"),
        "The prompt does not know about the reminder she holds.");

    Access::DeliverReminders(session, now + 5min);
    Access::DeliverReminders(session, now + 11min);
    {
        std::lock_guard lock(mutex);
        Check(said.size() == 1 && said.front() == "Reminder: stretch.",
            "The reminder was not delivered once, on time.");
    }
    Check(Contains(Access::SubmitOperator(session, "/reminders").text, "No reminders"),
        "A delivered reminder stayed on the list.");

    const auto unclear = Access::SubmitOperator(session, "remind me to call mom");
    Check(Contains(unclear.text, "Tell me when") &&
            Contains(Access::SubmitOperator(session, "/reminders").text, "No reminders"),
        "A reminder with no time was promised instead of questioned.");

    (void)Access::SubmitOperator(session, "set a timer for 1 minute");
    Access::DeliverReminders(session, now + 3h);
    {
        std::lock_guard lock(mutex);
        Check(said.size() == 2 && Contains(said.back(), "while I was offline"),
            "A reminder missed while she was closed was not delivered as late.");
    }
    session.Events().Unsubscribe(id);
}

} // namespace

void RunReminderTests()
{
    TestRemindersAreReadWithoutTheModel();
    TestTheBookKeepsThemAcrossRestarts();
    TestSheSetsAndDeliversThem();
    std::cout << "Reminders and timers are read without the model, survive restarts and "
        "arrive on time.\n";
}
