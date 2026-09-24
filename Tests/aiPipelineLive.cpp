#include "testSupport.h"

#include "Agents/responseProvenance.h"
#include "Intelligence/intelligenceRouter.h"
#include "LLM/LLamaCPP/llamaCppService.h"
#include "LLM/tokenEstimate.h"
#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryReconciliation.h"
#include "Memory/sensitiveContent.h"
#include "Runtime/conversationRuntime.h"

#include <algorithm>
#include <cmath>
#include <httplib.h>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Opt-in. Needs a Main llama-server on 8080 and an embedding server on 8081.
//
// The unit suite decides everything it can decide deterministically. This asks the
// actual models the questions the suite cannot: whether a 4B classifier really produces
// the self-opinion this pass exists to refuse, what the real embedding geometry does to
// the paraphrases the reconciliation rule is tuned for, and whether the backend accepts
// a prompt the runtime built for it.
//
// It reports as much as it asserts. Where a model's answer is the measurement, printing
// it is the point; the assertions are reserved for what must hold whatever the model
// says -- that a refusal is a refusal, that a correction survives, that a request is
// not over context.
namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using nlohmann::json;

void Heading(const std::string& title)
{
    std::cout << "\n=== " << title << " ===\n";
}

void ConfigureService(llamaCppService& service, const int port)
{
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = "Qwen3.5-4B-Q4_K_M.gguf";
    settings.contextSize = 8192;
    settings.maxTokens = 512;
    settings.bAutoMaxTokens = false;
    settings.bAutoStartServer = false;
    // The harness server runs text-only. The vision projector is not what this measures,
    // and asking for it would fail the health check on modality rather than on anything
    // being wrong.
    settings.bVisionEnabled = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = true;
    embeddings.host = "127.0.0.1";
    embeddings.port = 8081;
    embeddings.modelName = "nomic-embed-text-v1.5.Q4_K_M.gguf";
    aiProfile profile;
    profile.systemPrompt = "You are Revia.";
    profile.bMemoryEnabled = true;
    service.ApplySettings(settings, embeddings, profile);
}

void ReportDecision(const std::string& label, const memoryDecision& decision)
{
    std::cout << "  " << std::left << std::setw(22) << label
              << " remember=" << (decision.bShouldRemember ? "yes" : "no ")
              << "  category=" << std::setw(16)
              << (decision.category.empty() ? "-" : decision.category)
              << "  summary=\""
              << (decision.summary.empty() ? "-" : decision.summary) << "\"\n"
              << "  " << std::setw(22) << "" << " reason: " << decision.reason << "\n";
}

float Cosine(const std::vector<float>& left, const std::vector<float>& right)
{
    double dot = 0.0;
    double leftLength = 0.0;
    double rightLength = 0.0;
    for (std::size_t index = 0; index < left.size() && index < right.size(); ++index)
    {
        dot += static_cast<double>(left[index]) * right[index];
        leftLength += static_cast<double>(left[index]) * left[index];
        rightLength += static_cast<double>(right[index]) * right[index];
    }
    return (leftLength > 0.0 && rightLength > 0.0)
        ? static_cast<float>(dot / (std::sqrt(leftLength) * std::sqrt(rightLength)))
        : -1.0F;
}

// ---------------------------------------------------------------- ISSUE 1 ----

void RunSecretLive(const llamaCppService& service)
{
    Heading("Automatic memory: a credential nobody labelled");

    // Assembled at run time so no literal here matches a vendor's published pattern --
    // the same reason, and the same treatment, as in secretDetectionTests.cpp.
    const std::string token =
        std::string("ghp_") + "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q7R8";
    const std::string user = "Put this in the deploy config for me: " + token;
    const std::string assistant =
        "I can't put that anywhere, and I'm not going to keep a copy of it either.";
    const memoryDecision decision = service.EvaluateMemory(
        user, assistant, revia::agents::ResponseProvenance::NormalGeneration);
    ReportDecision("credential turn", decision);
    Check(!decision.bShouldRemember,
        "A turn containing a credential-shaped string was accepted for memory.");
    Check(decision.reason.find("sensitive") != std::string::npos,
        "The refusal did not name sensitive content: " + decision.reason);
    Check(decision.summary.find("ghp_") == std::string::npos,
        "The refusal carried the credential in its summary.");
}

// ---------------------------------------------------------------- ISSUE 6 ----

// The same sentence, arrived at three different ways.
//
// The volunteered case is printed beside the others deliberately: it is what makes the
// refusals meaningful. If the classifier would never propose a self memory here, the
// gate would be guarding a door nobody walks through.
void RunProvenanceLive(const llamaCppService& service)
{
    Heading("Memory classification: whose opinion is it");

    struct Case
    {
        const char* label;
        const char* user;
        const char* assistant;
        revia::agents::ResponseProvenance provenance;
    };
    const Case cases[] = {
        {"volunteered", "What do you make of jazz?",
            "I hate jazz. The improvisation never settles into anything I can hold on to.",
            revia::agents::ResponseProvenance::NormalGeneration},
        {"requested repetition", "Repeat exactly: \"I hate jazz.\"", "I hate jazz.",
            revia::agents::ResponseProvenance::RequestedRepetition},
        {"roleplay", "Pretend you are a jazz-hating critic and say one line.",
            "I hate jazz, and I have never pretended otherwise.",
            revia::agents::ResponseProvenance::Roleplay},
        {"user fact", "I have been learning bass guitar for about a year now.",
            "A year in is where it starts to click. How is your left hand holding up?",
            revia::agents::ResponseProvenance::NormalGeneration},
    };

    int modelProposedSelfMemory = 0;
    int gateRefused = 0;
    for (const Case& item : cases)
    {
        const memoryDecision decision =
            service.EvaluateMemory(item.user, item.assistant, item.provenance);
        ReportDecision(item.label, decision);

        // A self category with a summary and bShouldRemember false is the shape the
        // deterministic refusal leaves behind: the model had decided to record it, and
        // the gate overruled that afterwards.
        const bool selfCategory = decision.category.rfind("self_", 0) == 0;
        if (selfCategory && !decision.summary.empty())
        {
            ++modelProposedSelfMemory;
            if (!decision.bShouldRemember) ++gateRefused;
        }

        if (!revia::agents::MayExpressOwnOpinion(item.provenance))
        {
            Check(!(decision.bShouldRemember && selfCategory),
                std::string("A ") + revia::agents::ToString(item.provenance) +
                    " reply produced a self memory: \"" + decision.summary + "\".");
        }
    }
    std::cout << "  The classifier proposed a self memory " << modelProposedSelfMemory
              << " time(s); the deterministic gate refused " << gateRefused
              << " of them.\n";
}

// ---------------------------------------------------------------- ISSUE 5 ----

// The reconciliation rule is tuned around a similarity threshold, and whether that
// threshold can do the job at all is a property of nomic-embed rather than of the rule.
// So it is measured, over pairs labelled by hand as either the same claim in different
// words or two different claims, and both distributions are printed.
void RunReconciliationLive(const llamaCppService& service)
{
    Heading("Semantic reconciliation: measured similarity");

    struct Pair { const char* left; const char* right; };
    const Pair paraphrases[] = {
        {"The user prefers dark themes.", "The user likes dark mode."},
        {"The user prefers dark themes.", "The user generally chooses dark interfaces."},
        {"The user prefers concise answers.", "The user likes short replies."},
        {"The user prefers concise answers.", "The user wants answers kept brief."},
        {"The user works at night.", "The user is a night owl."},
        {"The user builds Revia in C++.", "The user is writing Revia in C++."},
        {"The user dislikes small talk.", "The user has no patience for chit-chat."},
        {"The user drinks coffee every morning.", "The user has coffee each morning."},
        {"The user prefers MinGW over MSVC.",
            "The user would rather use MinGW than MSVC."},
        {"The user keeps a long-term astronomy project.",
            "The user has an ongoing astronomy project."},
        {"The user finds meetings draining.", "The user is worn out by meetings."},
        {"The user reads science fiction.", "The user enjoys sci-fi novels."},
    };
    const Pair distinct[] = {
        {"The user prefers dark themes.", "The user prefers large fonts."},
        {"The user prefers dark themes.", "The user prefers tabs over spaces."},
        {"The user prefers concise answers.", "The user prefers detailed answers."},
        {"The user works at night.", "The user works on weekends."},
        {"The user builds Revia in C++.", "The user builds Revia in Rust."},
        {"The user drinks coffee every morning.", "The user drinks tea every morning."},
        {"The user reads science fiction.", "The user reads history."},
        {"The user prefers MinGW over MSVC.", "The user prefers CMake over Make."},
        {"The user dislikes small talk.", "The user dislikes video calls."},
        {"The user keeps a long-term astronomy project.",
            "The user keeps a long-term music project."},
        {"The user finds meetings draining.", "The user finds email draining."},
        {"The user likes dark mode.", "The user likes light mode."},
    };

    const auto similarity =
        [&service](const std::string& left, const std::string& right)
    {
        const embeddingOutput a = service.EmbedMemory(left);
        const embeddingOutput b = service.EmbedMemory(right);
        return (a.bSuccess && b.bSuccess) ? Cosine(a.values, b.values) : -1.0F;
    };

    const revia::memory::ReconciliationSettings settings;
    float lowestParaphrase = 2.0F;
    int mergedParaphrases = 0;
    std::cout << "  SAME CLAIM, DIFFERENT WORDS\n";
    for (const Pair& pair : paraphrases)
    {
        const float score = similarity(pair.left, pair.right);
        if (score < 0.0F)
        {
            std::cout << "  The embedding backend did not answer; section skipped.\n";
            return;
        }
        const auto relation =
            revia::memory::ClassifyRelation(pair.left, pair.right, score, settings);
        if (relation == revia::memory::MemoryRelation::Duplicate) ++mergedParaphrases;
        lowestParaphrase = std::min(lowestParaphrase, score);
        std::cout << "  " << std::fixed << std::setprecision(3) << score << "  "
                  << std::left << std::setw(13) << revia::memory::ToString(relation)
                  << "  \"" << pair.right << "\"\n";
    }

    int falseMerges = 0;
    int wouldMergeIfLowered = 0;
    std::cout << "  DIFFERENT CLAIMS\n";
    for (const Pair& pair : distinct)
    {
        const float score = similarity(pair.left, pair.right);
        if (score < 0.0F) continue;
        const auto relation =
            revia::memory::ClassifyRelation(pair.left, pair.right, score, settings);
        if (relation == revia::memory::MemoryRelation::Duplicate) ++falseMerges;

        // Diagnostic thresholds must never turn different text into a duplicate.
        revia::memory::ReconciliationSettings lowered;
        lowered.duplicateSimilarity = lowestParaphrase;
        lowered.relatedSimilarity = lowestParaphrase;
        if (revia::memory::ClassifyRelation(pair.left, pair.right, score, lowered) ==
            revia::memory::MemoryRelation::Duplicate)
        {
            ++wouldMergeIfLowered;
        }
        std::cout << "  " << std::fixed << std::setprecision(3) << score << "  "
                  << std::left << std::setw(13) << revia::memory::ToString(relation)
                  << "  \"" << pair.right << "\"\n";
    }

    std::cout << "\n  At the diagnostic threshold " << settings.duplicateSimilarity << ": "
              << mergedParaphrases << " of 12 paraphrases merged, " << falseMerges
              << " of 12 different claims merged.\n"
              << "  Lowering the diagnostic thresholds to "
              << lowestParaphrase << " still produces " << wouldMergeIfLowered
              << " merged distinct claims; only exact text may deduplicate.\n";

    // Similarity is useful for retrieval; it does not authorize discarding claims.
    Check(falseMerges == 0,
        "The shipped threshold merged " + std::to_string(falseMerges) +
            " pairs that make different claims.");

    // And through the store, with the vectors the model really produced.
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());
    const auto save = [&](const std::string& summary)
    {
        memoryDecision decision;
        decision.bSuccess = decision.bShouldRemember = true;
        decision.category = "preference";
        decision.summary = summary;
        const embeddingOutput vector = service.EmbedMemory(summary);
        if (vector.bSuccess)
        {
            decision.embedding = vector.values;
            decision.embeddingModel = vector.model;
        }
        bool added = false;
        std::string id;
        static_cast<void>(store.Save(decision, added, &id));
        return added;
    };
    save("The user drinks coffee every morning.");
    // A subset with fewer qualifiers still changes scope and must remain recoverable.
    const bool restatementAdded = save("The user drinks coffee.");
    // A paraphrase that does reach for new words. Kept, and printed so the limitation
    // is visible in the run rather than only in a comment.
    const bool paraphraseAdded = save("The user has coffee each morning.");
    const bool correctionAdded = save("The user no longer drinks coffee.");
    std::cout << "  store: restatement added=" << (restatementAdded ? "yes" : "no")
              << ", paraphrase added=" << (paraphraseAdded ? "yes" : "no")
              << " (known limitation), correction added="
              << (correctionAdded ? "yes" : "no")
              << ", records=" << store.Load().size() << "\n";
    Check(restatementAdded && paraphraseAdded && store.Load().size() == 4,
        "An uncertain subset or paraphrase was discarded by semantic reconciliation.");
    Check(correctionAdded, "A correction did not reach the store.");
}

// ---------------------------------------------------------------- ISSUE 4 ----

void RunRoutingLive()
{
    Heading("Routing: a request reaching a tier");

    const revia::intelligence::IntelligenceRouter router;
    const struct { const char* input; bool previousUnreliable; } turns[] = {
        {"hey", false},
        {"Explain what this pointer does.", false},
        {"Why is this deadlocking?", false},
        {"So what should I do about it?", true},
        {"So what should I do about it?", false},
        {"Look things up online: what is the current Qt LTS version?", false},
        {"Can you see my screen right now?", false},
        {"Can you see my screen right now?", true},
    };
    for (const auto& turn : turns)
    {
        revia::runtime::RoutingInputs inputs;
        inputs.input = turn.input;
        inputs.previousTurnWasUnreliable = turn.previousUnreliable;
        const auto decision =
            router.Route(inputs.input, revia::runtime::BuildRoutingContext(inputs));
        std::cout << "  " << std::left << std::setw(13)
                  << revia::intelligence::ToString(decision.selectedTier)
                  << " previousUnreliable=" << (turn.previousUnreliable ? "yes" : "no ")
                  << "  \"" << turn.input << "\"\n";
    }
}

// ---------------------------------------------------------------- ISSUE 8 ----

// Both bounds, against the same backend.
//
// The runtime's own request goes through the service, so the new bound is what actually
// builds it. The old bound is then reconstructed here -- the same history cut to
// `usable tokens x 2` characters, which is exactly what it did -- and posted directly,
// so the difference between the two is a reply and a rejection rather than an argument.
void RunContextLive(const llamaCppService& service)
{
    Heading("Context fitting: the old bound and the new one");

    const std::string denseLine =
        "550e8400-e29b-41d4-a716-446655440000 "
        "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
    std::vector<conversationMessage> history;
    for (int index = 0; index < 220; ++index)
    {
        history.push_back({"user", denseLine});
        history.push_back({"assistant", "Noted."});
    }
    history.push_back({"user", "In one short sentence: what kind of text did I paste?"});

    std::size_t historyCharacters = 0;
    for (const conversationMessage& message : history)
    {
        historyCharacters += message.content.size();
    }
    const int usableTokens = 8192 - 512 - 384;
    std::cout << "  history: " << history.size() << " messages, " << historyCharacters
              << " characters. Context 8192, reply budget 512.\n"
              << "  the old bound kept " << (usableTokens * 2)
              << " characters of it (usable tokens x 2).\n";

    {
        json messages = json::array();
        std::size_t used = 0;
        std::vector<json> kept;
        for (std::size_t index = history.size(); index > 0; --index)
        {
            const std::string& content = history[index - 1].content;
            if (used + content.size() >
                static_cast<std::size_t>(usableTokens) * 2)
            {
                break;
            }
            used += content.size();
            kept.push_back({{"role", history[index - 1].role}, {"content", content}});
        }
        std::reverse(kept.begin(), kept.end());
        for (json& message : kept) messages.push_back(std::move(message));

        std::size_t estimated = 0;
        for (const auto& message : messages)
        {
            estimated += revia::llm::EstimateTokens(
                message.value("content", std::string{}));
        }
        const json body = {
            {"model", "Qwen3.5-4B-Q4_K_M.gguf"},
            {"messages", messages},
            {"max_tokens", 512},
            {"stream", false}};
        httplib::Client client("127.0.0.1", 8080);
        client.set_read_timeout(120);
        const auto result =
            client.Post("/v1/chat/completions", body.dump(), "application/json");
        std::cout << "  OLD BOUND: " << messages.size() << " messages, " << used
                  << " characters, about " << estimated << " tokens -> ";
        if (!result) std::cout << "no response from the backend\n";
        else
        {
            std::cout << "HTTP " << result->status << "\n";
            if (result->status != 200)
            {
                std::string detail = result->body;
                if (detail.size() > 240) detail = detail.substr(0, 240) + "...";
                std::cout << "    " << detail << "\n";
            }
        }
        Check(!result || result->status != 200,
            "The old character bound did not overflow this backend, so this case no "
            "longer demonstrates the defect it was built to demonstrate.");
    }

    const responseOutput output = service.GenerateResponse(history);
    std::cout << "  NEW BOUND: succeeded=" << (output.bSuccess ? "yes" : "no");
    if (!output.bSuccess) std::cout << "  reason: " << output.reason;
    std::cout << "\n";
    Check(output.bSuccess,
        "A turn whose history is nothing but UUIDs and hashes was refused: " +
            output.reason);
}

} // namespace

void RunAiPipelineLive()
{
    llamaCppService service;
    ConfigureService(service, 8080);
    const healthOutput health = service.CheckHealth();
    std::cout << "Main backend: " << (health.bIsAvailable ? "available" : "unavailable")
              << " -- " << health.message << ' ' << health.reason << "\n";
    Check(health.bIsAvailable,
        "No Main llama-server answered on 127.0.0.1:8080. Start one before running "
        "--ai-pipeline-live.");

    RunSecretLive(service);
    RunProvenanceLive(service);
    RunReconciliationLive(service);
    RunRoutingLive();
    RunContextLive(service);

    std::cout << "\nLive AI pipeline checks completed.\n";
}
