#include "reviaSessionTestAccess.h"
#include "conversationRuntimeTestAccess.h"
#include "Planning/reminders.h"

#include <algorithm>
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

void TestRepeatsAndChecksAreRead()
{
    const auto now = JuneMorning();
    const ReminderRequest halfHourly = Parsed("remind me every 30 minutes to stretch", now);
    Check(halfHourly.repeatEvery == 30min && halfHourly.due - now == 30min &&
            halfHourly.text == "stretch" && !halfHourly.check &&
            revia::planning::Label(halfHourly) == "stretch (every 30 min)",
        "A half-hourly reminder was not read: " + revia::planning::Label(halfHourly));
    const ReminderRequest trailing = Parsed("remind me to drink water every hour", now);
    Check(trailing.repeatEvery == 1h && trailing.due - now == 1h && trailing.text == "drink water",
        "A repeat clause at the end was not read: " + revia::planning::Label(trailing));
    const ReminderRequest daily = Parsed("remind me every day at 9am to take my pills", now);
    Check(daily.repeatEvery == 24h && daily.due - now == 23h && daily.text == "take your pills",
        "A daily reminder at a clock time was not read: " + revia::planning::Label(daily));
    const ReminderRequest dailyTrailing = Parsed("remind me to take my pills at 9am every day", now);
    Check(dailyTrailing.repeatEvery == 24h && dailyTrailing.due - now == 23h &&
            dailyTrailing.text == "take your pills",
        "A daily reminder with the clock first was not read.");
    Check(Parsed("/remind every 30m stretch", now).repeatEvery == 30min &&
            Parsed("/remind every day at 9am take my pills", now).text == "take your pills" &&
            Parsed("remind me daily to write", now).repeatEvery == 24h,
        "The command and 'daily' forms were not read.");
    Check(revia::planning::DescribeRepeat(daily) == "every day" &&
            revia::planning::DescribeRepeat(trailing) == "every hour" &&
            revia::planning::DescribeRepeat(halfHourly) == "every 30 min" &&
            revia::planning::DescribeRepeat(Parsed("remind me weekly to water the plants", now)) == "every week",
        "Repeats are not described as expected.");

    const ReminderRequest weather = Parsed("check the weather in Boston every hour", now);
    Check(weather.check && weather.repeatEvery == 1h && weather.due - now == 1h &&
            weather.text == "the weather in Boston" &&
            revia::planning::Label(weather) == "check: the weather in Boston (every hour)",
        "An hourly check was not read: " + revia::planning::Label(weather));
    const ReminderRequest build = Parsed("check if the build is green in 20 minutes", now);
    Check(build.check && !build.Repeats() && build.due - now == 20min && build.text == "if the build is green",
        "A one-off check was not read: " + revia::planning::Label(build));
    const ReminderRequest order = Parsed("Revia, keep checking the order status every 2 hours", now);
    Check(order.check && order.repeatEvery == 2h && order.text == "the order status",
        "'keep checking' was not read: " + revia::planning::Label(order));
    const ReminderRequest release = Parsed("/check 30m is the release out?", now);
    Check(release.check && !release.Repeats() && release.due - now == 30min && release.text == "is the release out",
        "The command form of a check was not read: " + revia::planning::Label(release));
    const ReminderRequest morning = Parsed("/check every day at 9am is the release out", now);
    Check(morning.check && morning.repeatEvery == 24h && morning.due - now == 23h,
        "A daily check at a clock time was not read.");
    const ReminderRequest late = Parsed("check the scores at 3pm", now);
    Check(late.check && !late.Repeats() && late.due - now == 5h && late.text == "the scores",
        "A check at a clock time was not read.");

    ReminderRequest ignored;
    std::string error;
    Check(ParseReminderRequest("remind me every 10 seconds to blink", now, ignored, error) ==
            ReminderParse::Invalid && Contains(error, "once a minute"),
        "A ten-second repeat was accepted.");
    Check(ParseReminderRequest("check every minute if it rains", now, ignored, error) ==
            ReminderParse::Invalid && Contains(error, "every 5 minutes"),
        "A one-minute check was accepted.");
    for (const char* ordinary : {"check my email", "check in with me every hour",
        "can you check whether that's right", "check the oven"})
    {
        Check(ParseReminderRequest(ordinary, now, ignored, error) == ReminderParse::NotAReminder,
            std::string("Read as a check: ") + ordinary);
    }
    Check(ParseReminderRequest("/check", now, ignored, error) == ReminderParse::Invalid &&
            Contains(error, "how often"),
        "A bare /check did not explain itself.");
    Check(ParseReminderRequest("/check 10m", now, ignored, error) == ReminderParse::Invalid &&
            Contains(error, "What should I check"),
        "A check with no question was accepted.");
}

void TestRepeatingOnesComeBack()
{
    revia::tests::ScopedTestDirectory directory;
    const auto file = directory.root / "reminders.json";
    const auto now = JuneMorning();
    ReminderBook book;
    std::string error;
    Check(book.Initialize(file, error), "A new reminder book could not start.");
    ReminderRequest stretch;
    stretch.text = "stretch";
    stretch.due = now + 30min;
    stretch.repeatEvery = 30min;
    ReminderRequest weather;
    weather.text = "the weather";
    weather.check = true;
    weather.due = now + 2h;
    weather.repeatEvery = 1h;
    const auto stretchId = book.Add(stretch, error)->id;
    (void)book.Add(weather, error);

    std::vector<revia::planning::Reminder> due = book.TakeDue(now + 31min);
    Check(due.size() == 1 && due.front().request.text == "stretch" && due.front().request.Repeats(),
        "The half-hourly reminder was not delivered.");
    std::vector<revia::planning::Reminder> pending = book.Pending();
    Check(pending.size() == 2 && pending.front().request.text == "stretch" &&
            pending.front().request.due == now + 60min && pending.front().id == stretchId,
        "The delivered repeat did not come back for its next time under the same id.");

    // Missed for hours: back once, for the next time after now, not once per interval.
    due = book.TakeDue(now + 5h + 10min);
    Check(due.size() == 2, "Two overdue repeats were not both delivered once.");
    pending = book.Pending();
    Check(pending.size() == 2 && pending[0].request.due == now + 5h + 30min &&
            pending[1].request.due == now + 6h,
        "Overdue repeats did not come back for the next time past now.");

    ReminderBook reopened;
    Check(reopened.Initialize(file, error), "The book could not be reopened.");
    pending = reopened.Pending();
    Check(pending.size() == 2 && pending[0].request.repeatEvery == 30min && !pending[0].request.check &&
            pending[1].request.repeatEvery == 1h && pending[1].request.check,
        "Repeat and check did not survive a restart.");
    Check(reopened.Cancel(2).has_value() && reopened.Pending().size() == 1,
        "A repeating reminder could not be cancelled.");
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

void TestSheChecksWhatSheWasAsked()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    std::mutex mutex;
    std::vector<RuntimeEvent> said;
    std::vector<std::string> statuses;
    std::vector<std::string> lookups;
    const auto id = session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        std::lock_guard lock(mutex);
        if (event.kind == RuntimeEventKind::AssistantMessage && event.component == "Reminder")
        {
            said.push_back(event);
        }
        if (event.kind == RuntimeEventKind::ComponentStatus && event.component == "Reminder")
        {
            statuses.push_back(event.phase);
        }
    });
    revia::runtime::ConversationRuntime& runtime = Access::Conversation(session);
    revia::runtime::ConversationRuntimeTestAccess::SetInternetSettings(runtime, []
    {
        revia::actions::CapabilitySettings::InternetAccess access;
        access.enabled = true;
        access.automaticLookup = true;
        access.quarantinedReader = false;
        return access;
    });
    revia::runtime::ConversationRuntimeTestAccess::SetInternetLookup(runtime,
        [&](const std::string& query, const std::string&)
        {
            {
                std::lock_guard lock(mutex);
                lookups.push_back(query);
            }
            revia::actions::ActionOutcome outcome;
            outcome.result.succeeded = true;
            outcome.result.content = "Boston weather\nURL: https://example.test/boston\n"
                "Cloudy, 61 F.\nSource: https://example.test/boston";
            outcome.result.entries = {"https://example.test/boston"};
            outcome.result.message = "1 result";
            return outcome;
        });

    const auto now = WallClock::now();
    const auto set = Access::SubmitOperator(session, "check the weather in Boston every hour");
    Check(set.fromAssistant && Contains(set.text, "every hour") && Contains(set.text, "the weather in Boston"),
        "She did not confirm the check: " + set.text);
    Check(Contains(Access::SubmitOperator(session, "/reminders").text, "check: the weather in Boston (every hour)"),
        "The check is not listed.");

    // Not yet due: nothing runs.
    Access::MarkStarted(session, true);
    Access::DeliverReminders(session, now + 30min);
    Access::WaitForCheck(session);
    {
        std::lock_guard lock(mutex);
        Check(lookups.empty() && said.empty(), "A check ran before it was due.");
    }
    Access::DeliverReminders(session, now + 61min);
    Access::WaitForCheck(session);
    Access::MarkStarted(session, false);
    {
        std::lock_guard lock(mutex);
        Check(lookups.size() == 1 && Contains(lookups.front(), "Boston"),
            "The check did not look the question up: " +
                (lookups.empty() ? std::string("no lookup") : lookups.front()));
        Check(std::find(statuses.begin(), statuses.end(), "Checking") != statuses.end(),
            "The check was not announced on the activity feed.");
        // No model here, so the honest line; with one, the answer. Either way it is a
        // Reminder message and never a fabricated answer.
        Check(said.size() == 1 && said.front().phase == "check" &&
                (Contains(said.front().message, "couldn't check") ||
                    Contains(said.front().message, "not ready") ||
                    Contains(said.front().message, "Boston")),
            "The check's outcome was not delivered as a reminder message: " +
                (said.empty() ? std::string("nothing") : said.front().message));
    }
    const std::string listed = Access::SubmitOperator(session, "/reminders").text;
    Check(Contains(listed, "check: the weather in Boston (every hour)"),
        "The hourly check did not come back after running: " + listed);
    Check(Contains(Access::SubmitOperator(session, "/reminders cancel 1").text, "Cancelled"),
        "The check could not be cancelled.");
    session.Events().Unsubscribe(id);
}

} // namespace

void RunReminderTests()
{
    TestRemindersAreReadWithoutTheModel();
    TestRepeatsAndChecksAreRead();
    TestTheBookKeepsThemAcrossRestarts();
    TestRepeatingOnesComeBack();
    TestSheSetsAndDeliversThem();
    TestSheChecksWhatSheWasAsked();
    std::cout << "Reminders, timers, repeats and checks are read without the model, survive "
        "restarts, arrive on time and come back when they repeat.\n";
}
