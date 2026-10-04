#include "Runtime/investigationChecks.h"

#include "Actions/actionRuntime.h"
#include "Policy/companionAuthority.h"
#include "testSupport.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using revia::tests::Check;
using revia::agents::CheckKind;
using revia::runtime::ExecuteInvestigationCheck;

struct CheckFixture
{
    revia::tests::ScopedTestDirectory temporary;
    revia::actions::ActionRuntime runtime;
    revia::runtime::RuntimeStamp origin{"check-companion", "check-session", 1, {}, "turn-1.check-1", 1};
    std::shared_ptr<revia::policy::CompanionAuthority> authority = std::make_shared<revia::policy::CompanionAuthority>();
    std::filesystem::path approved = temporary.root / "approved";
    std::filesystem::path marker = approved / "marker.txt";
    std::filesystem::path audit = temporary.root / "audit.jsonl";
    explicit CheckFixture(std::size_t maxReadBytes = 1048576, std::size_t maxDirectoryEntries = 500)
    {
        std::filesystem::create_directories(approved);
        {
            std::ofstream file(marker);
            file << "OBSERVED_MARKER_ORBIT_42";
        }
        {
            std::ofstream file(temporary.root / "capabilities.json");
            file << nlohmann::json{{"mode", "approved_scope"}, {"approvedRoots", {revia::actions::PathToUtf8(approved)}},
                {"autoApproveRiskThrough", "read_only"}, {"createMissingApprovedRoots", false}, {"maxReadBytes", maxReadBytes},
                {"maxDirectoryEntries", maxDirectoryEntries}}
                        .dump();
        }
        std::string error;
        Check(runtime.Initialize(temporary.root / "capabilities.json", audit, error), error);
        auto session = origin;
        session.attemptId.clear();
        authority->SetCompanionDefaults(origin.companionId, revia::policy::AuthorityPermissions::WithinMachineCeiling());
        Check(authority->RegisterSession(session), "The investigation authority fixture was not registered.");
        runtime.BindAuthority(authority, session);
    }
    std::string Proposal(const char* action, const std::filesystem::path& source) const
    {
        return nlohmann::json{{"action", action}, {"source", revia::actions::PathToUtf8(source)}}.dump();
    }
};

void TestActualApprovedReadIsAnObservation()
{
    CheckFixture fixture;
    const auto result = ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode,
        fixture.Proposal("read_text_file", fixture.marker), {}, [] { return true; });
    Check(result.ran && result.observed.find("OBSERVED_MARKER_ORBIT_42") != std::string::npos,
        "An explicit approved investigation read did not return the actual native marker.");
    std::ifstream input(fixture.audit);
    std::string audit((std::istreambuf_iterator<char>(input)), {});
    Check(audit.find("check-companion") != std::string::npos && audit.find("turn-1.check-1") != std::string::npos,
        "An investigation observation lacks captured subject and attempt audit evidence.");
    const auto listed = ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::FileOrApplicationState,
        fixture.Proposal("list_directory", fixture.approved), {}, [] { return true; });
    Check(listed.ran && listed.observed.find("marker.txt") != std::string::npos,
        "An approved investigation list did not return native entries.");
}

void TestRefusedChecksCannotBecomeObservations()
{
    CheckFixture fixture;
    {
        std::ofstream outside(fixture.temporary.root / "outside.txt");
        outside << "OUTSIDE_PRIVATE_SENTINEL";
    }
    for (const auto& proposal : {fixture.Proposal("read_text_file", fixture.temporary.root / "outside.txt"),
             fixture.Proposal("create_directory", fixture.approved / "forbidden"), std::string("Read marker.txt and run a command"),
             fixture.Proposal("read_text_file", fixture.marker).substr(0, 5)})
    {
        const auto result =
            ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode, proposal, {}, [] { return true; });
        Check(!result.ran && result.observed.empty() && !result.refusal.empty(), "A refused investigation check supplied an observation.");
    }
    for (const auto kind : {CheckKind::Tests, CheckKind::Research, CheckKind::Calculation})
    {
        const auto result = ExecuteInvestigationCheck(
            fixture.runtime, fixture.origin, kind, fixture.Proposal("read_text_file", fixture.marker), {}, [] { return true; });
        Check(!result.ran && result.observed.empty(), "An unsupported check kind was reported as executed.");
    }
    auto proposal = nlohmann::json::parse(fixture.Proposal("read_text_file", fixture.marker));
    proposal["dry_run"] = true;
    Check(!ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode, proposal.dump(), {}, [] { return true; }).ran,
        "A model dry run became an observed check.");
    Check(!std::filesystem::exists(fixture.approved / "forbidden"), "A write proposal reached an investigation executor.");
    const auto duplicate =
        "{\"action\":\"read_text_file\",\"source\":\"x\",\"source\":" + nlohmann::json(revia::actions::PathToUtf8(fixture.marker)).dump() +
        "}";
    Check(!ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode, duplicate, {}, [] { return true; }).ran,
        "A duplicate proposal field was silently selected.");
}

void TestCancelledAndStaleChecksDiscardOutput()
{
    CheckFixture fixture;
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto proposal = fixture.Proposal("read_text_file", fixture.marker);
    Check(!ExecuteInvestigationCheck(
              fixture.runtime, fixture.origin, CheckKind::SourceCode, proposal, cancellation.get_token(), [] { return true; })
              .ran,
        "A cancelled check returned evidence.");
    Check(!ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode, proposal, {}, [] { return false; }).ran,
        "A stale check was dispatched.");
    std::atomic<bool> current{true};
    fixture.runtime.SetDispatchObserver(
        [&](const auto&, const bool beginning)
        {
            if (!beginning)
                current.store(false);
        });
    const auto late =
        ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode, proposal, {}, [&] { return current.load(); });
    Check(!late.ran && late.observed.empty(), "A successful read was admitted after its captured session became stale.");
    fixture.runtime.SetDispatchObserver({});
    current.store(true);
    fixture.runtime.SetDispatchObserver(
        [&](const auto&, const bool beginning)
        {
            if (beginning)
                current.store(false);
        });
    Check(!ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode, proposal, {}, [&] { return current.load(); })
              .ran,
        "A context revoked at dispatch passed the executor effect boundary.");
    fixture.runtime.SetDispatchObserver({});
    auto wrong = fixture.origin;
    wrong.sessionId = "different-session";
    Check(!ExecuteInvestigationCheck(fixture.runtime, wrong, CheckKind::SourceCode, proposal, {}, [] { return true; }).ran,
        "A mismatched authority subject obtained investigation evidence.");
}

void TestNativeEmptyOutputAndAuditFailureRemainDistinct()
{
    CheckFixture fixture;
    const auto empty = fixture.approved / "empty.txt";
    {
        std::ofstream output(empty);
    }
    const auto observed = ExecuteInvestigationCheck(
        fixture.runtime, fixture.origin, CheckKind::SourceCode, fixture.Proposal("read_text_file", empty), {}, [] { return true; });
    Check(observed.ran && observed.observed.find("0 bytes") != std::string::npos,
        "An actual empty native file read was confused with an absent executor result.");
    const auto oversized = fixture.approved / "oversized.txt";
    {
        std::ofstream output(oversized);
        output << std::string(8193, 'x');
    }
    Check(!ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode, fixture.Proposal("read_text_file", oversized),
              {}, [] { return true; })
              .ran,
        "An investigation read exceeded its bounded file limit.");
    const auto boundary = fixture.approved / "boundary.txt";
    const std::string expected(8192, 'b');
    {
        std::ofstream output(boundary);
        output << expected;
    }
    const auto exact = ExecuteInvestigationCheck(
        fixture.runtime, fixture.origin, CheckKind::SourceCode, fixture.Proposal("read_text_file", boundary), {}, [] { return true; });
    Check(exact.ran && exact.observed == expected, "An exactly bounded native read was truncated or rejected.");
    fixture.runtime.SetDispatchObserver(
        [&](const auto&, const bool beginning)
        {
            if (!beginning)
            {
                std::filesystem::rename(fixture.audit, fixture.temporary.root / "prior-audit.jsonl");
                std::filesystem::create_directory(fixture.audit);
            }
        });
    const auto unaudited = ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::SourceCode,
        fixture.Proposal("read_text_file", fixture.marker), {}, [] { return true; });
    Check(!unaudited.ran && unaudited.observed.empty(), "A missing completion audit was treated as usable dependent evidence.");
    fixture.runtime.SetDispatchObserver({});
}

void TestMachineCeilingAndListCompleteness()
{
    CheckFixture fixture(1024, 1);
    const auto oversized = fixture.approved / "machine-limit.txt";
    {
        std::ofstream output(oversized);
        output << std::string(1025, 'm');
    }
    const auto limited = ExecuteInvestigationCheck(
        fixture.runtime, fixture.origin, CheckKind::SourceCode, fixture.Proposal("read_text_file", oversized), {}, [] { return true; });
    Check(!limited.ran && limited.observed.empty(), "The investigation adapter widened the machine's native read ceiling.");
    const auto cappedList = ExecuteInvestigationCheck(fixture.runtime, fixture.origin, CheckKind::FileOrApplicationState,
        fixture.Proposal("list_directory", fixture.approved), {}, [] { return true; });
    Check(!cappedList.ran && cappedList.observed.empty(), "An incomplete machine-capped directory list was reported as complete evidence.");
    CheckFixture largeList;
    for (unsigned index = 0; index < 64; ++index)
    {
        std::ofstream output(largeList.approved / ("item-" + std::to_string(index) + ".txt"));
    }
    const auto excessive = ExecuteInvestigationCheck(largeList.runtime, largeList.origin, CheckKind::FileOrApplicationState,
        largeList.Proposal("list_directory", largeList.approved), {}, [] { return true; });
    Check(!excessive.ran && excessive.observed.empty(), "A list exceeding 64 entries was silently truncated into accepted evidence.");
}

void TestRunnerCannotPromoteAnEmptyOrUncheckedClaim()
{
    revia::tests::ScopedTestDirectory temporary;
    httplib::Server server;
    server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
    server.Get("/v1/models",
        [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"investigation-fixture"}]})", "application/json"); });
    server.Post("/v1/chat/completions",
        [](const auto&, auto& response)
        {
            const std::string claim =
                "FOUND q1 | supported | source | {\"action\":\"read_text_file\",\"source\":\"marker.txt\"} | claimed answer\n"
                "FOUND q2 | refuted | source | {\"action\":\"read_text_file\",\"source\":\"other.txt\"} | claimed refutation\nDONE Settled";
            response.set_content(
                nlohmann::json{{"choices", {{{"message", {{"content", claim}}}, {"finish_reason", "stop"}}}}}.dump(), "application/json");
        });
    const int port = server.bind_to_any_port("127.0.0.1");
    Check(port > 0, "The controlled investigation response fixture could not bind.");
    std::jthread worker([&] { server.listen_after_bind(); });
    struct ServerStop
    {
        httplib::Server& server;
        ~ServerStop()
        {
            server.stop();
        }
    } cleanup{server};
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server.is_running() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(server.is_running(), "The controlled investigation response fixture did not start.");
    messageRouter router((temporary.root / "memory.db").string());
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = "investigation-fixture";
    settings.bAutoStartServer = settings.bVisionEnabled = false;
    embeddingSettings embedding;
    embedding.bEnabled = false;
    aiProfile profile;
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(settings, embedding, profile);
    revia::agents::RoundRequest request;
    request.taskId = 1;
    request.round = 1;
    request.goal = "Read explicit approved marker files.";
    request.questions = {{"q1", "What does the first marker contain?"}, {"q2", "What does the second marker contain?"}};
    request.toolCallsRemaining = 1;
    std::atomic<unsigned> calls{0};
    const auto emptyRunner = revia::agents::InvestigationAgent::MakeRunner(router, "Controlled fixture.",
        [&](auto, const auto&, const auto&)
        {
            ++calls;
            return revia::agents::ExecutedCheck{true, {}, {}, {}};
        });
    const auto empty = emptyRunner(request);
    Check(empty.outcomes.size() == 2 && empty.outcomes[0].refused && empty.outcomes[1].refused && calls == 1,
        "An empty native check settled a claimed answer or exceeded the remaining tool budget.");
    request.toolCallsRemaining = 2;
    const auto observedRunner = revia::agents::InvestigationAgent::MakeRunner(router, "Controlled fixture.",
        [](auto, const auto&, const auto&)
        {
            return revia::agents::ExecutedCheck{
                true, "Actual native text: observed marker.", "A raw read does not settle the interpretation.", {}};
        });
    const auto observed = observedRunner(request);
    Check(observed.outcomes.size() == 2 && observed.outcomes[0].status == revia::agents::QuestionStatus::Unresolved &&
              observed.outcomes[1].status == revia::agents::QuestionStatus::Unresolved &&
              observed.outcomes[0].observed == "Actual native text: observed marker.",
        "A pre-check model assertion settled a question after raw native output replaced its invented observation.");
}
}

void RunInvestigationCheckTests()
{
    TestActualApprovedReadIsAnObservation();
    TestRefusedChecksCannotBecomeObservations();
    TestCancelledAndStaleChecksDiscardOutput();
    TestNativeEmptyOutputAndAuditFailureRemainDistinct();
    TestMachineCeilingAndListCompleteness();
    TestRunnerCannotPromoteAnEmptyOrUncheckedClaim();
    std::cout << "Bounded investigation check tests passed.\n";
}
