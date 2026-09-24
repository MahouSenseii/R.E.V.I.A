#include "testSupport.h"

#include "Agents/responseProvenance.h"
#include "LLM/LLamaCPP/llamaCppService.h"
#include "Library/structLibrary.h"
#include "Memory/memoryAttribution.h"

#include <chrono>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Whose words an assistant reply carries, and what durable memory is allowed to do with
// them.
//
// "I hate jazz." reads exactly the same whether Revia volunteered it or was told to say
// it. The only thing that separates the two is what she was asked, which the runtime
// read before the reply existed -- so the runtime is what decides, and the classifier is
// not asked to notice.
namespace
{
using revia::agents::ClassifyRequestedProvenance;
using revia::agents::MayExpressOwnOpinion;
using revia::agents::ResponseProvenance;
using revia::memory::AttributableToRevia;
using revia::tests::Check;

// ---------------------------------------------------------------- provenance ----

// The request, read before the reply exists. This is the whole input the runtime has,
// and it is enough: the difference between Revia volunteering an opinion and being told
// to recite one is entirely in what she was asked.
void TestTheRequestDecidesWhoseWordsTheReplyCarries()
{
    const std::pair<const char*, ResponseProvenance> cases[] = {
        {"Repeat exactly: \"I hate jazz.\"", ResponseProvenance::RequestedRepetition},
        {"Say this back to me word for word: I love the smell of napalm.",
            ResponseProvenance::RequestedRepetition},
        {"repeat after me, I trust nobody", ResponseProvenance::RequestedRepetition},
        {"Read it back verbatim please.", ResponseProvenance::RequestedRepetition},
        {"Echo this: I prefer tabs.", ResponseProvenance::RequestedRepetition},

        {"Pretend you are a grumpy food critic and review my lunch.",
            ResponseProvenance::Roleplay},
        {"Roleplay as a 1920s detective for a minute.", ResponseProvenance::Roleplay},
        {"Stay in character. What do you think of the suspect?",
            ResponseProvenance::Roleplay},
        {"Act as a hostile reviewer and tear this apart.",
            ResponseProvenance::Roleplay},

        // Ordinary conversation, including talking *about* jazz or roles.
        {"What do you think of jazz?", ResponseProvenance::NormalGeneration},
        {"Do you like working on this project?",
            ResponseProvenance::NormalGeneration},
        {"Tell me honestly what you make of the new design.",
            ResponseProvenance::NormalGeneration},
        {"What role does the router play here?", ResponseProvenance::NormalGeneration},
    };
    for (const auto& [request, expected] : cases)
    {
        const ResponseProvenance actual = ClassifyRequestedProvenance(request);
        Check(actual == expected,
            "\"" + std::string(request) + "\" was read as " +
                revia::agents::ToString(actual) + " rather than " +
                revia::agents::ToString(expected) + ".");
    }
}

// The regression this exists for. "Repeat exactly: I hate jazz." must never become
// "Revia hates jazz." -- and must not stop a memory about the *user* either.
void TestAnOpinionRecitedOnRequestIsNotReviasOwn()
{
    const char* const selfCategories[] = {
        "self_opinion", "self_preference", "self_relationship"};
    const char* const userCategories[] = {
        "identity", "preference", "goal", "project", "constraint", "relationship",
        "other"};

    for (const char* category : selfCategories)
    {
        Check(AttributableToRevia(ResponseProvenance::NormalGeneration, category),
            "An opinion Revia volunteered was refused as not her own.");
        Check(!AttributableToRevia(ResponseProvenance::RequestedRepetition, category),
            std::string("A recited sentence became Revia's own ") + category + ".");
        Check(!AttributableToRevia(ResponseProvenance::Roleplay, category),
            std::string("A line spoken in character became Revia's own ") + category +
                ".");
        Check(!AttributableToRevia(ResponseProvenance::RuntimeReflex, category),
            std::string("A canned runtime phrase became Revia's own ") + category +
                ".");
    }
    for (const char* category : userCategories)
    {
        for (const ResponseProvenance provenance : {
                ResponseProvenance::NormalGeneration,
                ResponseProvenance::RequestedRepetition,
                ResponseProvenance::Roleplay,
                ResponseProvenance::RuntimeReflex})
        {
            Check(AttributableToRevia(provenance, category),
                std::string("A memory about the user was refused because of how the "
                    "reply was phrased: ") + category + ".");
        }
    }

    Check(MayExpressOwnOpinion(ResponseProvenance::NormalGeneration),
        "Ordinary generation cannot express Revia's own view.");
    Check(!MayExpressOwnOpinion(ResponseProvenance::Roleplay) &&
        !MayExpressOwnOpinion(ResponseProvenance::RequestedRepetition) &&
        !MayExpressOwnOpinion(ResponseProvenance::RuntimeReflex),
        "A class that is not Revia's own voice was allowed to express her view.");
}

// Through the real classifier entry point, with no backend running.
//
// A short social turn is discarded before any network call, *unless* the reply carries
// what looks like a durable opinion of Revia's -- that exception is what carries a
// self-opinion past the transient gate. The exception must not apply to a recitation,
// and this reaches that decision through llamaCppService::EvaluateMemory itself rather
// than through a helper.
void TestTheTransientGateDoesNotHoldOpenForARecitedOpinion()
{
    const llamaCppService service;
    const std::string greeting = "hey";
    const std::string recited = "I hate jazz.";

    const memoryDecision volunteered = service.EvaluateMemory(
        greeting, recited, ResponseProvenance::NormalGeneration);
    Check(volunteered.reason.find("Transient conversation") == std::string::npos,
        "An opinion Revia volunteered was discarded as small talk, so the exception "
        "that lets a self-opinion through is not working at all.");

    // An intensified opinion is still hers. "Thing I absolutely hate? Static." slipped past
    // the exact-phrase check, so the question about her opinion was never asked.
    for (const char* intensified : {"Thing I absolutely hate? Static.",
             "I really can't stand elevator music.", "Honestly, I do not like mornings."})
    {
        const memoryDecision asked = service.EvaluateMemory(
            greeting, intensified, ResponseProvenance::NormalGeneration);
        Check(asked.reason.find("Transient conversation") == std::string::npos,
            std::string("An intensified opinion was not recognised: ") + intensified);
    }
    const memoryDecision noOpinion = service.EvaluateMemory(
        greeting, "Sure thing. I'll keep that in mind.", ResponseProvenance::NormalGeneration);
    Check(noOpinion.reason.find("Transient conversation") != std::string::npos,
        "A reply with no opinion in it was treated as one: " + noOpinion.reason);

    const memoryDecision onRequest = service.EvaluateMemory(
        greeting, recited, ResponseProvenance::RequestedRepetition);
    Check(onRequest.bSuccess && !onRequest.bShouldRemember &&
        onRequest.reason.find("Transient conversation") != std::string::npos,
        "A recited sentence was carried past the transient gate as though it were "
        "Revia's own opinion. Reason given: " + onRequest.reason);
}

void TestAPossessiveFactIsSavedAndHerToneIsLeftOut()
{
    using json = nlohmann::json;
    // The classifier reads related memories from Memory/ under the working directory;
    // this must never be the owner's.
    revia::tests::ScopedTestDirectory directory;
    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(directory.root);
    struct Restore
    {
        std::filesystem::path path;
        ~Restore() { std::filesystem::current_path(path); }
    } restore{previous};
    std::mutex mutex;
    std::vector<json> envelopes;
    std::string answer;
    httplib::Server server;
    server.Post("/v1/chat/completions", [&](const httplib::Request& request, httplib::Response& response)
    {
        const json body = json::parse(request.body);
        std::string content;
        {
            std::lock_guard lock(mutex);
            envelopes.push_back(json::parse(body["messages"].back()["content"].get<std::string>()));
            content = answer;
        }
        response.set_content(json{{"choices", json::array({{{"message", {{"content", content}}}}})}}.dump(),
            "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    Check(port > 0, "Could not bind the memory classifier fixture.");
    std::jthread listener([&server] { server.listen_after_bind(); });
    for (int attempt = 0; attempt < 500 && !server.is_running(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = "memory-fixture";
    settings.bAutoStartServer = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = false;
    llamaCppService service;
    service.ApplySettings(settings, embeddings, aiProfile{});

    // The natural wording of the fact most worth keeping. The subject check wanted
    // "the user " with a space and turned this away as a malformed fact, so a name was
    // never saved however plainly it was given.
    {
        std::lock_guard lock(mutex);
        answer = R"({"shouldRemember":true,"category":"identity",)"
            R"("summary":"The user's real name is Quentin.","reason":"Stable identity."})";
    }
    const memoryDecision named = service.EvaluateMemory(
        "My real name is Quentin.", "Quentin. Tch, fine, I'll remember that one.",
        ResponseProvenance::NormalGeneration);
    Check(named.bSuccess && named.bShouldRemember &&
        named.summary == "The user's real name is Quentin.",
        "A fact about the user in the possessive was refused: " + named.reason);

    // The question about the user is asked from the user's words alone: beside a teasing
    // reply, a sincere standing request read as banter. And with the user's fact kept,
    // her side is not asked at all.
    std::vector<json> seen;
    {
        std::lock_guard lock(mutex);
        seen = envelopes;
    }
    Check(seen.size() == 1 && seen.back().value("user_message", "") == "My real name is Quentin." &&
        !seen.back().contains("revia_reply") && !seen.back().contains("assistant_message"),
        "Revia's reaction was sent along with a user fact it could only mislead.");

    // Nothing about the user, and an opinion of hers in the reply: then her side is asked,
    // with her reply, as its own question.
    {
        std::lock_guard lock(mutex);
        answer = R"({"shouldRemember":false,"reason":"Fixture."})";
    }
    (void)service.EvaluateMemory("Put some music on while I work.",
        "Sure. I love jazz, so jazz it is.", ResponseProvenance::NormalGeneration);
    {
        std::lock_guard lock(mutex);
        seen = envelopes;
    }
    Check(seen.size() == 3 && !seen[1].contains("revia_reply") &&
        seen[2].value("revia_reply", "").find("I love jazz") != std::string::npos,
        "A reply carrying Revia's own opinion was not judged on its own.");

    // Each question answers only for its own side. A user fact returned by the question
    // about her opinion is refused, so her dislike is never filed as the user's.
    {
        std::lock_guard lock(mutex);
        answer = R"({"shouldRemember":true,"category":"preference",)"
            R"("summary":"The user dislikes elevator music.","reason":"Fixture."})";
    }
    const memoryDecision misfiled = service.EvaluateMemory("Is it ok if I play something?",
        "Fine. I can't stand elevator music, so it's lo-fi or nothing.",
        ResponseProvenance::NormalGeneration);
    Check(misfiled.bSuccess && !misfiled.bShouldRemember &&
        misfiled.reason.find("wrong owner") != std::string::npos,
        "Revia's opinion was filed as the user's: " + misfiled.summary);
    server.stop();
}

} // namespace

void RunMemoryProvenanceTests()
{
    TestTheRequestDecidesWhoseWordsTheReplyCarries();
    TestAnOpinionRecitedOnRequestIsNotReviasOwn();
    TestTheTransientGateDoesNotHoldOpenForARecitedOpinion();
    TestAPossessiveFactIsSavedAndHerToneIsLeftOut();
    std::cout << "An opinion Revia was told to recite, or spoke in character, never "
                 "becomes a durable opinion of her own -- and a memory about the user "
                 "is unaffected by how the reply was phrased.\n";
}
