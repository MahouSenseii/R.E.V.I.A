#include "testSupport.h"

#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryReconciliation.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

// Similar vectors are useful for retrieval, but cannot establish claim equivalence.
// Semantic paraphrase accumulation and contradiction supersession remain open:
// ISSUE-REVIA-0079 and ISSUE-REVIA-0080.
namespace
{
using revia::memory::ClassifyRelation;
using revia::memory::MemoryRelation;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

memoryDecision Decision(const std::string& summary, std::vector<float> embedding = {})
{
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.category = "preference";
    decision.summary = summary;
    if (!embedding.empty())
    {
        decision.embedding = std::move(embedding);
        decision.embeddingModel = "fixture";
    }
    return decision;
}

// A vector at a chosen angle from (1,0), so a case can state the similarity it means to
// test instead of depending on a real embedding model's geometry.
std::vector<float> AtSimilarity(const double cosine)
{
    return {static_cast<float>(cosine),
        static_cast<float>(std::sqrt(std::max(0.0, 1.0 - cosine * cosine)))};
}

void TestDistinctClaimsSurviveSimilarVectors()
{
    const std::pair<const char*, const char*> cases[] = {
        {"The user prefers coffee to tea.", "The user prefers tea to coffee."},
        {"Revia likes jazz.", "The user likes jazz."},
        {"The user's mother knows their father.", "The user's father knows their mother."},
        {"The user no longer likes tea but likes coffee.",
            "The user no longer likes coffee but likes tea."},
        {"The user always drinks coffee.", "The user usually drinks coffee."},
        {"The user says their sister trusts their sister.",
            "The user says their sister trusts their brother."},
        {"The user drinks coffee every morning.", "The user drinks coffee."},
        {"The user may choose tea.", "The user must choose tea."},
        {"The user checks tea then coffee then tea.", "The user checks tea then coffee."}
    };
    for (const auto& [stored, candidate] : cases)
    {
        for (const float similarity : {0.81F, 0.90F, 0.99F})
        {
            Check(ClassifyRelation(stored, candidate, similarity) != MemoryRelation::Duplicate,
                "Different propositions were classified as duplicates: " + std::string(candidate));
            ScopedTestDirectory directory;
            const std::string path = (directory.root / "memory.db").string();
            std::string firstId, secondId;
            {
                longTermMemory store(path);
                bool added = false;
                Check(store.Save(Decision(stored, AtSimilarity(1.0)), added, &firstId) && added,
                    "Could not save the first distinct proposition.");
                Check(store.Save(Decision(candidate, AtSimilarity(similarity)), added, &secondId) && added,
                    "A similar vector discarded a distinct proposition: " + std::string(candidate));
            }
            longTermMemory reopened(path);
            const auto entries = reopened.Load();
            Check(firstId != secondId && entries.size() == 2,
                "Distinct propositions did not survive reopening the memory store.");
            for (const std::string text : {stored, candidate})
                Check(std::any_of(entries.begin(), entries.end(), [&text](const memoryEntry& entry)
                    { return entry.summary == text; }), "A durable claim was lost: " + text);
        }
    }
}

void TestParaphrasesAndCorrectionsAreToldApart()
{
    struct Case
    {
        const char* stored;
        const char* candidate;
        float similarity;
        MemoryRelation expected;
        const char* note;
    };
    const Case cases[] = {
        // Exact text is equivalent; changed wording is uncertain and retained.
        {"The user prefers dark themes.", "The user prefers dark themes.",
            0.10F, MemoryRelation::Duplicate, "exact text despite dissimilar vectors"},
        {"The user prefers dark themes.", "The user generally chooses dark themes.",
            0.97F, MemoryRelation::Refinement, "quantification changed"},
        {"The user prefers concise answers.", "The user likes concise answers.", 0.98F,
            MemoryRelation::Refinement, "preference verb swapped"},
        {"The user prefers concise technical answers.",
            "The user prefers concise answers.", 0.97F,
            MemoryRelation::Refinement, "a narrower claim became general"},

        // Saying more is not saying it again.
        {"The user prefers concise answers.",
            "The user prefers concise answers when debugging.", 0.95F,
            MemoryRelation::Refinement, "a condition added"},

        // Opposite claims can score above the similarity threshold.
        {"The user prefers concise answers.", "The user prefers detailed answers.",
            0.944F, MemoryRelation::Refinement, "opposite adjective, measured live"},
        {"The user likes dark mode.", "The user likes light mode.", 0.939F,
            MemoryRelation::Refinement, "opposite adjective, measured live"},
        // And the near misses from the same run, which the threshold alone was within
        // four thousandths of merging.
        {"The user drinks coffee every morning.",
            "The user drinks tea every morning.", 0.926F,
            MemoryRelation::Unrelated, "different drink, measured live at 0.926"},
        {"The user keeps a long-term astronomy project.",
            "The user keeps a long-term music project.", 0.929F,
            MemoryRelation::Unrelated, "different project, measured live at 0.929"},

        // A paraphrase that reaches for a different word is kept, not merged. This is
        // the accumulation case that is still open, asserted here so the limitation is
        // recorded rather than assumed away.
        {"The user prefers dark themes.", "The user likes dark mode.", 0.96F,
            MemoryRelation::Refinement, "paraphrase this rule does not catch"},

        // Corrections. None of these may be swallowed, however close the vectors are.
        {"The user likes coffee.", "The user does not like coffee.", 0.99F,
            MemoryRelation::Contradiction, "negation"},
        {"The user likes coffee.", "The user no longer likes coffee.", 0.99F,
            MemoryRelation::Contradiction, "no longer"},
        {"The user drinks coffee.", "The user never drinks coffee.", 0.99F,
            MemoryRelation::Contradiction, "never"},
        {"The user drinks coffee.", "The user stopped drinking coffee.", 0.99F,
            MemoryRelation::Contradiction, "stopped"},
        {"The user drinks coffee.", "The user drinks tea instead of coffee.", 0.99F,
            MemoryRelation::Contradiction, "instead of"},
        {"The user drinks coffee.", "The user changed from coffee to tea.", 0.99F,
            MemoryRelation::Contradiction, "changed from"},

        // Different things, however similar the sentences look.
        {"The user uses C++.", "The user uses C#.", 0.99F,
            MemoryRelation::Unrelated, "punctuated identifiers differ"},
        {"The user uses .NET.", "The user uses NET.", 0.99F,
            MemoryRelation::Unrelated, "a dot is part of the name"},
        {"The user uses version 1.2.3.", "The user uses version 12.3.", 0.99F,
            MemoryRelation::Unrelated, "version numbers differ"},
        {"The offset is -10.", "The offset is 10.", 0.99F,
            MemoryRelation::Unrelated, "sign differs"},
        {"Alice trusts Bob.", "Bob trusts Alice.", 0.99F,
            MemoryRelation::Unrelated, "same names, opposite relationship"},

        // Same area, different claim. Similarity alone must not be enough.
        {"The user prefers dark themes.", "The user prefers large fonts.", 0.88F,
            MemoryRelation::Unrelated, "related but distinct preference"},
        {"The user prefers dark themes.", "The user builds Revia in C++.", 0.20F,
            MemoryRelation::Unrelated, "nothing in common"},
    };

    for (const Case& item : cases)
    {
        const MemoryRelation actual =
            ClassifyRelation(item.stored, item.candidate, item.similarity);
        Check(actual == item.expected,
            std::string("\"") + item.stored + "\" vs \"" + item.candidate +
                "\" was read as " + revia::memory::ToString(actual) +
                " rather than " + revia::memory::ToString(item.expected) + " (" +
                item.note + ").");
    }
}

// The store keeps uncertain paraphrases and corrections, and deduplicates exact text.
void TestTheStoreKeepsUncertainClaimsAndEveryCorrection()
{
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());
    bool added = false;
    std::string first, second, third, correction;

    Check(store.Save(Decision("The user prefers dark themes.", AtSimilarity(1.0)),
        added, &first) && added, "The first memory was not saved.");
    Check(store.Save(Decision("The user generally chooses dark themes.",
        AtSimilarity(0.99)), added, &second),
        "An uncertain restatement could not be saved.");
    Check(added && second != first,
        "An uncertain restatement was discarded as a duplicate.");
    Check(store.Save(Decision("The user prefers dark themes.",
        AtSimilarity(0.995)), added, &third), "A third restatement failed.");
    Check(!added && third == first, "A third restatement became another record.");
    Check(store.Load().size() == 2,
        "Two distinct summaries produced " + std::to_string(store.Load().size()) + " records.");

    // A correction must survive even when its vector is almost identical.
    Check(store.Save(Decision("The user no longer likes dark themes.",
        AtSimilarity(0.999)), added, &correction) && added,
        "A correction was discarded as a duplicate of what it corrects.");
    Check(store.Load().size() == 3,
        "The store did not keep both preferences and the correction.");

    // And an opposite carrying no negation word, which is the case the live run found.
    std::string opposite;
    Check(store.Save(Decision("The user prefers light themes.", AtSimilarity(0.998)),
        added, &opposite) && added,
        "A preference was merged with its opposite because the vectors were close.");
    Check(store.Load().size() == 4,
        "The store did not keep the opposite preference as its own record.");

    // And nothing already written was removed to make room for it.
    const auto entries = store.Load();
    const bool originalSurvives = std::any_of(entries.begin(), entries.end(),
        [&first](const memoryEntry& entry) { return entry.id == first; });
    Check(originalSurvives,
        "Reconciliation deleted the older record. A contradiction is kept beside what "
        "it contradicts, never in place of it.");
}

// Without a vector there is nothing to reconcile, and the exact rule stands alone.
void TestAMemoryWithNoVectorIsNeverReconciled()
{
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());
    bool added = false;
    std::string first, second;
    Check(store.Save(Decision("The user prefers dark themes."), added, &first) && added,
        "The first memory was not saved.");
    Check(store.Save(Decision("The user generally chooses dark themes."), added,
        &second) && added,
        "A restatement with no vector was discarded on a similarity nobody measured.");
    Check(store.Load().size() == 2, "The unreconciled store did not keep both.");
}

} // namespace

void RunMemoryDedupLimitsTests()
{
    TestDistinctClaimsSurviveSimilarVectors();
    TestParaphrasesAndCorrectionsAreToldApart();
    TestTheStoreKeepsUncertainClaimsAndEveryCorrection();
    TestAMemoryWithNoVectorIsNeverReconciled();
    std::cout << "Exact memory repeats deduplicate; uncertain semantic candidates and "
                 "distinct durable claims remain recoverable.\n";
}
