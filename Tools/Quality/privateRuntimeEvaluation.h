#pragma once

#include "Memory/conversationArchive.h"
#include "Runtime/conversationRuntime.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace revia::quality
{
// Optional single-turn cohort setup; expected answers never enter the runtime.
class PrivateRuntimeEvaluation
{
public:
    explicit PrivateRuntimeEvaluation(const nlohmann::json& corpus) : cases(corpus.at("cases")) {}

    void Prepare(const std::string& input, const std::vector<conversationMessage>& prior, conversationContext& history)
    {
        if (!prior.empty())
            throw std::runtime_error("Private runtime cohorts require one scored turn per case.");
        current = nullptr;
        for (const auto& item : cases)
        {
            if (item.at("turns").size() == 1 && item.at("turns").front().at("input") == input)
            {
                if (current)
                    throw std::runtime_error("Private runtime cohort inputs must be unique.");
                current = &item;
            }
        }
        if (!current)
            throw std::runtime_error("Private runtime case setup is missing.");
        history.Clear();
        std::vector<conversationMessage> context;
        for (const auto& message : current->value("privateContext", nlohmann::json::array()))
            context.push_back({message.at("role").get<std::string>(), message.at("content").get<std::string>()});
        if (current->value("restoreArchive", false))
        {
            memory::MemoryScope scope;
            scope.participantId = "evaluation-owner";
            scope.companionId = "evaluation-companion";
            scope.participantSource = identity::SpeakerSource::ExplicitIntroduction;
            scope.consentRevision = 1;
            scope.audience = {identity::AudienceKind::Private, "evaluation-private", 1, {"evaluation-owner"}};
            const auto path = std::filesystem::absolute("cohort-archive-" + std::to_string(++archiveIndex) + ".db");
            if (std::filesystem::exists(path))
                throw std::runtime_error("Archive evaluation requires a fresh database.");
            {
                memory::ConversationArchive archive(path.string());
                std::string error;
                if (!archive.BeginSession("before-restart", error))
                    throw std::runtime_error("Cannot create evaluation archive: " + error);
                for (const auto& message : context)
                    if (!archive.Record("before-restart", message.role, message.content, error, scope))
                        throw std::runtime_error("Cannot persist evaluation context: " + error);
                if (!archive.EndSession("before-restart"))
                    throw std::runtime_error("Cannot close evaluation archive session.");
            }
            memory::ConversationArchive reopened(path.string());
            const auto retained = reopened.LoadLatestCompatibleTail("after-restart", 200, scope);
            if (retained.size() != context.size())
                throw std::runtime_error("Archive evaluation lost an admitted fixture turn.");
            context.clear();
            for (const auto& message : retained)
                context.push_back({message.role, message.content});
            history.RestoreMessages(context, 2);
        }
        else
        {
            for (const auto& message : context)
                history.AddMessage(message);
        }
        observations.push_back({{"caseId", current->at("id")}, {"restoredArchive", current->value("restoreArchive", false)},
            {"setupMessages", context.size()}, {"recentMessages", history.GetRecentMessages().size()},
            {"compressedHistory", history.GetCompressedHistorySummary()}, {"lookups", nlohmann::json::array()}});
    }

    [[nodiscard]] actions::CapabilitySettings::InternetAccess InternetSettings() const
    {
        actions::CapabilitySettings::InternetAccess access;
        access.enabled = current && current->contains("researchFixture");
        access.automaticLookup = access.enabled;
        return access;
    }

    [[nodiscard]] actions::ActionOutcome Lookup(const std::string& query, const std::string& origin)
    {
        if (!current || !current->contains("researchFixture"))
            throw std::runtime_error("Unexpected evaluation lookup without frozen source evidence.");
        const auto& source = current->at("researchFixture");
        observations.back()["lookups"].push_back({{"query", query}, {"origin", origin}, {"source", source.at("url")}});
        actions::ActionOutcome result;
        result.result.succeeded = true;
        result.result.content = "Frozen official source captured " + source.at("retrievedAt").get<std::string>() +
            "\nSource: " + source.at("url").get<std::string>() + "\n" + source.at("text").get<std::string>();
        result.result.entries = {source.at("url").get<std::string>()};
        result.result.message = "Frozen source replay for paired claim-support evaluation; no live browser request.";
        result.result.backend = "evaluation-source-replay";
        return result;
    }

    [[nodiscard]] runtime::SessionResult Reply(messageRouter& router, conversationContext& history,
        const std::string& input, const aiProfile& profile)
    {
        agents::TurnCoordinator coordinator;
        speech::SpeechService speech;
        runtime::AffectController affect;
        emotion::EmotionRuntime emotions;
        runtime::RuntimeEventBus events;
        logger log;
        runtime::ConversationRuntime turn(router, history, coordinator, speech, affect, emotions, events, log,
            [](runtime::RuntimeState, const std::string&) {}, [](const runtime::AffectSnapshot&) {},
            [this] { return InternetSettings(); }, [] { return actions::CapabilitySettings::DesktopControl{}; },
            [this](const std::string& query, const std::string& origin) { return Lookup(query, origin); },
            []
            {
                responseFilterSettings filters;
                filters.bAiReviewEnabled = false;
                return filters;
            }, [] { return std::string{}; });
        return turn.Reply(input, profile, true, false);
    }

    [[nodiscard]] const nlohmann::json& Observations() const { return observations; }

private:
    nlohmann::json cases;
    const nlohmann::json* current = nullptr;
    std::size_t archiveIndex = 0;
    nlohmann::json observations = nlohmann::json::array();
};
}
