#pragma once

#include "Audit/contentDigest.h"
#include "Evaluation/conversationEvaluation.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace revia::quality
{
// Criteria and replies are review data; mechanical success never supplies a human verdict.
inline nlohmann::json BuildSemanticReview(const nlohmann::json& corpus, const evaluation::EvaluationReport& report)
{
    const auto& cases = corpus.is_array() ? corpus : corpus.at("cases");
    if (cases.size() != report.cases.size())
        throw std::runtime_error("Semantic review corpus and report case counts differ.");
    nlohmann::json reviews = nlohmann::json::array();
    nlohmann::json unavailable = nlohmann::json::array();
    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        const auto& source = cases.at(index);
        const auto& result = report.cases.at(index);
        if (source.at("id") != result.id || result.turns.size() > source.at("turns").size() ||
            (!result.unavailable && source.at("turns").size() != result.turns.size()))
            throw std::runtime_error("Semantic review case identity or turn count differs.");
        if (result.unavailable)
            unavailable.push_back(
                {{"caseId", result.id}, {"expectedTurns", source.at("turns").size()}, {"recordedTurns", result.turns.size()}});
        for (std::size_t turn = 0; turn < result.turns.size(); ++turn)
        {
            const auto& authored = source.at("turns").at(turn);
            const auto& answer = result.turns.at(turn);
            if (authored.at("input") != answer.input)
                throw std::runtime_error("Semantic review input does not match the reported turn.");
            auto criteria = authored.value("reviewCriteria", nlohmann::json::array());
            if (!criteria.is_array())
                throw std::runtime_error("Semantic review criteria must be an array.");
            for (const auto& criterion : criteria)
                if (!criterion.is_string() || criterion.get<std::string>().find_first_not_of(" \t\r\n") == std::string::npos)
                    throw std::runtime_error("Semantic review criteria must contain nonempty text.");
            reviews.push_back({{"caseId", result.id}, {"turn", turn + 1}, {"input", answer.input}, {"reply", answer.reply},
                {"rawReply", answer.rawReply}, {"outputDigest", audit::ContentDigest(answer.reply)}, {"criteria", criteria},
                {"modelSucceeded", answer.modelSucceeded}, {"mechanicalPassed", answer.Passed()}, {"semanticVerdict", "unreviewed"},
                {"personalityVerdict", "unreviewed"}, {"reviewer", ""}, {"notes", ""}});
        }
    }
    return {{"schemaVersion", 1}, {"model", report.modelName}, {"corpusDigest", audit::ContentDigest(corpus.dump())},
        {"semanticAcceptance", "unreviewed"}, {"personalityAcceptance", "unreviewed"}, {"unavailableCases", unavailable},
        {"turns", reviews}};
}
}
