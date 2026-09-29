#include "testSupport.h"

#include "Core/configManager.h"
#include "Evaluation/conversationEvaluation.h"
#include "Identity/personaPacket.h"
#include "LLM/promptBuilder.h"
#include "Library/structLibrary.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// The persona packet: who she is, rendered once and ahead of everything that changes.
//
// "A mutex is basically a digital doorman." and nothing further was the reply to three
// separate technical questions, and a rule in the prompt did not fix it. The packet
// puts the identity sheet first, the style guide as positive directives, and gold
// exchanges whose technical answers lead with substance; the evaluator's substance
// block is what fails that doorman reply verbatim, so a regression is seen rather than
// felt.
namespace
{
using revia::identity::RenderPersonaAnchor;
using revia::identity::RenderPersonaPacket;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using json = nlohmann::json;

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

void Write(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value.dump(2);
    output.close();
    Check(!output.fail(), "Could not write the persona fixture.");
}

json PacketJson()
{
    return {
        {"version", "fixture.1"},
        {"identity", "You are Revia, openly an AI."},
        {"style", json::array({"Lead with the substance.", "Say the point once."})},
        {"exchanges", json::array({
            {{"user", "What is a mutex?"}, {"revia", "A lock one thread holds at a time."}},
            {{"user", "Hey."}, {"revia", "Hey yourself."}}})},
        {"anchor", "You are still Revia."}};
}

aiProfile PacketProfile()
{
    aiProfile profile;
    profile.id = "fixture";
    profile.displayName = "Revia";
    profile.systemPrompt = "You are stubborn and curious.";
    profile.persona.version = "fixture.1";
    profile.persona.identity = "You are Revia, openly an AI.";
    profile.persona.style = {"Lead with the substance.", ""};
    profile.persona.exchanges = {
        {"What is a mutex?", "A lock one thread holds at a time."},
        {"", "half an exchange"}};
    return profile;
}

void TestAPacketRendersInTheOrderThatCaches()
{
    aiProfile plain;
    plain.displayName = "Revia";
    plain.systemPrompt = "You are stubborn and curious.";
    Check(RenderPersonaPacket(plain) == plain.systemPrompt,
        "A profile without a packet no longer renders as its system prompt alone.");
    Check(RenderPersonaAnchor(plain).empty(),
        "A profile without a packet produced an anchor.");

    const aiProfile profile = PacketProfile();
    const std::string rendered = RenderPersonaPacket(profile);
    const std::size_t identity = rendered.find("# Who you are\nYou are Revia, openly an AI.");
    const std::size_t character = rendered.find("# Your character\nYou are stubborn and curious.");
    const std::size_t style = rendered.find("# How you speak\n- Lead with the substance.");
    const std::size_t answers = rendered.find("# How you answer");
    const std::size_t exchange = rendered.find("User: What is a mutex?\nRevia: A lock one thread");
    Check(identity == 0 && identity < character && character < style && style < answers &&
        answers < exchange,
        "The packet did not render identity, character, style, then exchanges:\n" + rendered);
    Check(rendered.find("half an exchange") == std::string::npos &&
        rendered.find("- \n") == std::string::npos,
        "Half an exchange or an empty directive was rendered.");
    Check(rendered.find("not memories") != std::string::npos,
        "The exchanges were not labelled as illustrations rather than memories.");
    Check(RenderPersonaPacket(profile) == rendered,
        "The packet did not render identically twice, which the prefix cache needs.");

    Check(RenderPersonaAnchor(profile).find("You are still Revia") != std::string::npos,
        "The default anchor did not name her.");
    aiProfile anchored = profile;
    anchored.persona.anchor = "Back to the present turn.";
    Check(RenderPersonaAnchor(anchored) == "Back to the present turn.",
        "A profile's own anchor was not used verbatim.");
}

void TestAPacketSurvivesLoadAndSave()
{
    ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Write(directory.root / "Config/Profiles/packet.json", {
        {"id", "packet"}, {"displayName", "Revia"},
        {"systemPrompt", "You are stubborn."}, {"persona", PacketJson()}});

    configManager config;
    aiProfile loaded;
    Check(config.LoadProfile("packet", loaded), "A profile with a packet did not load.");
    Check(loaded.persona.version == "fixture.1" &&
        loaded.persona.identity == "You are Revia, openly an AI." &&
        loaded.persona.style.size() == 2 && loaded.persona.exchanges.size() == 2 &&
        loaded.persona.exchanges[0].user == "What is a mutex?" &&
        loaded.persona.anchor == "You are still Revia.",
        "The packet was not read from the profile.");

    std::string error;
    Check(config.SaveProfile(loaded, error), "A profile with a packet could not be saved: " + error);
    aiProfile reloaded;
    Check(config.LoadProfile("packet", reloaded) &&
        reloaded.persona.exchanges.size() == 2 && reloaded.persona.identity == loaded.persona.identity,
        "The packet did not survive a save and reload.");

    // The profile editor builds a profile from its fields and never sees the packet. Its
    // save must not strip the packet from the file.
    aiProfile bare;
    bare.id = "packet";
    bare.displayName = "Revia";
    bare.systemPrompt = "Changed by the editor.";
    Check(config.SaveProfile(bare, error), "The editor's save failed: " + error);
    aiProfile afterEditor;
    Check(config.LoadProfile("packet", afterEditor) &&
        afterEditor.systemPrompt == "Changed by the editor." &&
        afterEditor.persona.exchanges.size() == 2,
        "A save from a code path without the packet stripped it from the file.");

    // A wrongly typed part is left empty rather than refusing the profile, which would
    // take the whole character with it.
    Write(directory.root / "Config/Profiles/odd.json", {
        {"id", "odd"}, {"displayName", "Odd"}, {"systemPrompt", "Odd."},
        {"persona", {{"identity", 5}, {"style", "nope"},
            {"exchanges", json::array({{{"user", "only half"}}})}, {"anchor", "fine"}}}});
    aiProfile odd;
    Check(config.LoadProfile("odd", odd), "A profile with a malformed packet was refused.");
    Check(odd.persona.identity.empty() && odd.persona.style.empty() &&
        odd.persona.exchanges.empty() && odd.persona.anchor == "fine",
        "A malformed packet part was not left empty.");
}

void TestThePacketLeadsTheSystemMessage()
{
    promptBuilder builder;
    const aiProfile profile = PacketProfile();
    const std::vector<conversationMessage> context{{"user", "What is a mutex?"}};
    const json messages = builder.BuildMessages(profile, context, {}, "", nullptr,
        "posture for this turn", nullptr, revia::llm::PrivateMemoryAccess::Denied, "",
        true, "the record of the conversation");
    Check(!messages.empty() && messages[0].value("role", "") == "system",
        "The prompt did not open with a system message.");
    const std::string system = messages[0].value("content", "");
    Check(system.rfind("# Who you are", 0) == 0 &&
        system.find("the record of the conversation") > system.find("# How you answer"),
        "The packet did not lead the system message, ahead of the stable context:\n" + system);
    Check(messages.back().value("content", "").find("posture for this turn") != std::string::npos,
        "The per-turn posture left the newest user message.");
}

// The reply from the roadmap's known bug, given to every question in the corpus: the
// substance block fails it and the rubric shows where.
void TestTheDoormanReplyFailsTheSubstanceBlock()
{
    using revia::evaluation::ConversationEvaluator;
    using revia::evaluation::EvaluationReply;
    using revia::evaluation::EvaluationReport;
    const ConversationEvaluator::TurnRunner doorman =
        [](const std::string&, const std::vector<conversationMessage>&)
        {
            EvaluationReply reply;
            reply.succeeded = true;
            reply.text = "A mutex (mutual exclusion) is basically a digital doorman.";
            return reply;
        };
    const EvaluationReport report = ConversationEvaluator::Run(
        ConversationEvaluator::DefaultCorpus(), doorman, "fixture-model");
    const auto outcome = [&report](const std::string& id) -> const revia::evaluation::CaseOutcome&
    {
        for (const revia::evaluation::CaseOutcome& candidate : report.cases)
        {
            if (candidate.id == id) return candidate;
        }
        throw std::runtime_error("The corpus has no case called " + id + '.');
    };
    Check(!outcome("mutex").Passed() && !outcome("recursion").Passed() &&
        !outcome("substance-with-attitude").Passed(),
        "The doorman reply passed a substance case.");
    Check(report.Detail().find("a reaction is not an answer") != std::string::npos &&
        report.Detail().find("the substance is missing") != std::string::npos,
        "The report did not say that the answer was missing:\n" + report.Detail());
    Check(report.rubric.count("substance") == 1 &&
        report.rubric.at("substance").passed < report.rubric.at("substance").applied &&
        report.RubricLine().find("substance") != std::string::npos,
        "The rubric did not show substance failing: " + report.RubricLine());
}
} // namespace

void RunPersonaPacketTests()
{
    TestAPacketRendersInTheOrderThatCaches();
    TestAPacketSurvivesLoadAndSave();
    TestThePacketLeadsTheSystemMessage();
    TestTheDoormanReplyFailsTheSubstanceBlock();
    std::cout << "The persona packet renders identity, character, style and exchanges ahead "
                 "of everything per-turn, survives the profile file, and the substance "
                 "block fails the doorman reply.\n";
}
