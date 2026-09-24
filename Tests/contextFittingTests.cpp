#include "testSupport.h"

#include "LLM/tokenEstimate.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

// Whether a prompt built to fill the context actually fits in it.
//
// The numbers in TokenCounts below are not guesses and are not this estimator's own
// output. They were produced by running llama-tokenize against
// Models/Qwen3.5-4B-Q4_K_M.gguf -- the model Config/settings.json configures for the
// Main tier -- over exactly the strings this file builds. They are the ground truth the
// estimator is checked against, and if the configured tokenizer is ever replaced they
// have to be measured again rather than adjusted until the suite passes.
namespace
{
using revia::llm::CompactToTokenBudget;
using revia::llm::EstimateTokens;
using revia::tests::Check;

std::string Repeat(const std::string& unit, const int times)
{
    std::string text;
    text.reserve(unit.size() * static_cast<std::size_t>(times));
    for (int index = 0; index < times; ++index) text += unit;
    return text;
}

struct Sample
{
    const char* name;
    std::string text;
    // Tokens llama-tokenize reported for this exact string.
    std::size_t measuredTokens;
    // Bytes per token that implies, for the record.
    double measuredDensity;
};

std::vector<Sample> Corpus()
{
    const std::string newline(1, '\n');
    return {
        {"english_prose",
            Repeat("The router chooses a tier before generation begins, using semantic "
                   "signals rather than message length. A short question can be hard "
                   "and a long request for a story can be easy. ", 30),
            1021, 5.17},
        {"cpp_source",
            Repeat("    if (!decision.bSuccess || !decision.bShouldRemember || "
                   "decision.summary.empty())\n    {\n        return false;\n    }\n"
                   "    const std::vector<float>& v = decision.embedding;\n"
                   "    for (std::size_t i = 0; i < v.size(); ++i) { sum += v[i] * "
                   "v[i]; }\n", 20),
            1480, 3.30},
        {"dense_punctuation",
            Repeat("}{;)(][<>!@#$%^&*~`|\\/?.,:'\"+=-_", 120), 2520, 1.52},
        {"random_hex",
            Repeat("9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
                20),
            1180, 1.08},
        {"random_base64",
            Repeat("aGVsbG8gd29ybGQgdGhpcyBpcyBhIHRlc3Qgb2YgYmFzZTY0IGVuY29kaW5n", 20),
            901, 1.33},
        {"uuid_list",
            Repeat("550e8400-e29b-41d4-a716-446655440000" + newline, 40), 1400, 1.06},
        {"emoji_heavy",
            Repeat("\U0001F600\U0001F680\U0001F9E0\U0001F4BB\U0001F525", 150),
            1800, 1.67},
        {"cjk",
            Repeat("今日はいい天気です。"
                   "今天天气很好。", 80),
            720, 5.67},
        {"json_blob",
            Repeat("{\"id\":\"a1\",\"v\":[1,2,3],\"n\":{\"k\":\"x\"}},", 100),
            2001, 1.90},
        {"minified_js",
            Repeat("function a(b,c){return b?c:!b&&c||a(b-1,c+1)};", 60), 1380, 2.00},
    };
}

// The reproduction. Six of these ten sit under two bytes per token, so the bound that
// assumed two could build a prompt the model refuses.
void TestTheOldTwoBytesPerTokenAssumptionIsMeasurablyWrong()
{
    int denserThanTheOldAssumption = 0;
    double densest = 99.0;
    for (const Sample& sample : Corpus())
    {
        const double density = static_cast<double>(sample.text.size()) /
            static_cast<double>(sample.measuredTokens);
        // The corpus is reproduced exactly, so the recorded density has to match what
        // this build constructs. A mismatch means the strings drifted from what was
        // measured, and the ground truth no longer describes them.
        Check(density > sample.measuredDensity - 0.05 &&
            density < sample.measuredDensity + 0.05,
            std::string("The ") + sample.name + " sample no longer matches the string "
            "that was tokenized: recorded " + std::to_string(sample.measuredDensity) +
            " bytes/token, built " + std::to_string(density) + ".");
        if (density < 2.0) ++denserThanTheOldAssumption;
        densest = std::min(densest, density);
    }
    Check(denserThanTheOldAssumption >= 6,
        "The corpus no longer demonstrates the defect it was built to demonstrate.");
    Check(densest < 1.1,
        "The corpus no longer contains a case dense enough to overflow a budget that "
        "assumes two bytes per token.");
}

// The estimator must never say a text is cheaper than it is. An over-estimate costs
// history; an under-estimate costs the whole request.
void TestTheEstimatorNeverUnderCountsTheRealTokenizer()
{
    double worstOver = 1.0;
    for (const Sample& sample : Corpus())
    {
        const std::size_t estimated = EstimateTokens(sample.text);
        Check(estimated >= sample.measuredTokens,
            std::string("The estimator under-counted ") + sample.name + ": estimated " +
                std::to_string(estimated) + " against " +
                std::to_string(sample.measuredTokens) + " real tokens. A prompt built "
                "on that estimate is over context and the request is refused.");
        worstOver = std::max(worstOver,
            static_cast<double>(estimated) /
                static_cast<double>(sample.measuredTokens));
    }
    // The reviewed heuristic spent more history but was not a safe upper bound.
    // Exact model/template counting can recover that efficiency in a future change;
    // the fallback deliberately pays up to one token per byte today.
    Check(worstOver <= 6.0, "The byte allowance unexpectedly exceeded the measured corpus bound.");
}

// Compaction has to land inside the budget it was given, not near it.
void TestCompactionLandsInsideItsBudget()
{
    const std::string marker = "\n[compacted]\n";
    for (const Sample& sample : Corpus())
    {
        for (const std::size_t budget : {std::size_t{16}, std::size_t{64},
                std::size_t{256}, std::size_t{1000}})
        {
            const std::string compacted =
                CompactToTokenBudget(sample.text, budget, marker);
            Check(EstimateTokens(compacted) <= budget,
                std::string("Compacting ") + sample.name + " to " +
                    std::to_string(budget) + " tokens produced " +
                    std::to_string(EstimateTokens(compacted)) + ".");
            Check(!compacted.empty(), "Compaction produced nothing at all.");
        }
    }
    // Text that already fits is returned untouched.
    const std::string small = "A short line.";
    Check(CompactToTokenBudget(small, 500, marker) == small,
        "Text that already fits was compacted anyway.");
    Check(CompactToTokenBudget(small, 0, marker).empty(),
        "A zero budget produced content.");
}

void TestWhitespaceAndRareTextHaveAnIndependentByteBound()
{
    for (const std::string& text : {std::string(12000, ' '), std::string(12000, '\n'),
        Repeat(" \t\n", 4000), "hello" + Repeat(" \t\n", 4000) + "world",
        Repeat("qzxv_jkQZ", 100), Repeat("\xE4\xB8\xAD\xF0\x9F\x98\x80", 100)})
    {
        Check(EstimateTokens(text) >= text.size(),
            "The conservative byte-token allowance under-counted text or whitespace.");
        for (const std::size_t budget : {std::size_t{1}, std::size_t{4}, std::size_t{31}, std::size_t{256}})
        {
            const auto compacted = CompactToTokenBudget(text, budget, "\n[compacted]\n");
            // A separate resource assertion, not an estimate checked by itself.
            Check(compacted.size() <= budget,
                "Compaction retained more UTF-8 bytes than the conservative token allowance.");
        }
    }
}

// The estimator runs on every turn, so its cost is part of time-to-first-token.
void TestEstimationCostIsNegligible()
{
    // About the size of a full context of prose.
    const std::string large = Repeat(Corpus().front().text, 12);
    const auto started = std::chrono::steady_clock::now();
    std::size_t total = 0;
    for (int pass = 0; pass < 20; ++pass) total += EstimateTokens(large);
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count() / 20.0;
    Check(total > 0, "The estimator returned nothing.");
    std::cout << "  Token estimate over " << large.size() << " bytes: "
              << elapsed << " ms per pass.\n";
    Check(elapsed < 50.0,
        "Estimating a full context took " + std::to_string(elapsed) +
            " ms, which is no longer negligible against time-to-first-token.");
}

// Every byte the compactor emits has to still be text.
//
// The estimator counts by codepoint but the compactor cut by byte, so a budget that
// landed inside a three-byte CJK character or a four-byte emoji produced a string that
// is not UTF-8 at all. nlohmann::json throws on that, and the request is built on the
// conversation worker -- so a long enough Chinese message could take the desktop app
// down. The suite missed it because every case here measured size and none looked at
// what the bytes were.
bool IsValidUtf8(const std::string& text)
{
    std::size_t index = 0;
    while (index < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead < 0x80) length = 1;
        else if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        else return false;
        if (index + length > text.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80)
            {
                return false;
            }
        }
        index += length;
    }
    return true;
}

void TestCompactionNeverSplitsACharacter()
{
    const std::string marker = "\n[compacted]\n";
    for (const Sample& sample : Corpus())
    {
        // Every budget in a wide range, because the defect only shows when the cut
        // happens to land mid-character -- one or two budgets would miss it by luck.
        for (std::size_t budget = 1; budget <= 400; ++budget)
        {
            const std::string compacted =
                CompactToTokenBudget(sample.text, budget, marker);
            Check(IsValidUtf8(compacted),
                std::string("Compacting ") + sample.name + " to " +
                    std::to_string(budget) + " tokens split a character and produced "
                    "invalid UTF-8.");
            Check(EstimateTokens(compacted) <= budget,
                std::string("Compacting ") + sample.name + " to " +
                    std::to_string(budget) + " tokens exceeded its budget.");
        }
    }
}

// The whole point: the compacted text has to survive being put in a request.
void TestCompactedMultilingualTextSerializes()
{
    const std::string marker = "\n[compacted]\n";
    for (const Sample& sample : Corpus())
    {
        for (const std::size_t budget : {std::size_t{7}, std::size_t{33},
                std::size_t{101}, std::size_t{257}})
        {
            const std::string compacted =
                CompactToTokenBudget(sample.text, budget, marker);
            const nlohmann::json body = {
                {"messages", nlohmann::json::array({
                    {{"role", "user"}, {"content", compacted}}})}};
            try
            {
                const std::string serialized = body.dump();
                Check(!serialized.empty(), "Serialization produced nothing.");
            }
            catch (const std::exception& error)
            {
                Check(false,
                    std::string("Compacted ") + sample.name + " at budget " +
                        std::to_string(budget) + " could not be serialized: " +
                        error.what());
            }
        }
    }
}

} // namespace

void RunContextFittingTests()
{
    TestWhitespaceAndRareTextHaveAnIndependentByteBound();
    TestTheOldTwoBytesPerTokenAssumptionIsMeasurablyWrong();
    TestTheEstimatorNeverUnderCountsTheRealTokenizer();
    TestCompactionLandsInsideItsBudget();
    TestCompactionNeverSplitsACharacter();
    TestCompactedMultilingualTextSerializes();
    TestEstimationCostIsNegligible();
    std::cout << "Context fitting is measured against the configured tokenizer: the "
                 "estimate never under-counts it, compaction lands inside its budget, "
                 "and whitespace consumes an independent byte allowance.\n";
}
