#include "Agents/agentDeliverable.h"

#include "Agents/agentWorkflow.h"
#include "Core/utf8.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>

namespace revia::agents
{
namespace
{
bool Text(const std::string& value, const std::size_t limit)
{
    return !value.empty() && value.size() <= limit && utf8::IsValid(value) &&
           std::any_of(value.begin(), value.end(), [](const unsigned char ch) { return !std::isspace(ch); }) &&
           std::none_of(
               value.begin(), value.end(), [](const unsigned char ch) { return ch < 32 && ch != '\n' && ch != '\r' && ch != '\t'; });
}

bool SameReference(const ArtifactReference& first, const ArtifactReference& second)
{
    return first.nodeId == second.nodeId && first.id == second.id && first.version == second.version && first.hash == second.hash;
}

bool AbsenceRationale(const std::string& value)
{
    if (!Text(value, 1024))
        return false;
    std::string label;
    for (const unsigned char ch : value)
        if (std::isalnum(ch))
            label.push_back(static_cast<char>(std::tolower(ch)));
    return label != "none" && label != "na" && label != "notapplicable" && label != "norisk" && label != "norisks" &&
           label != "noknownrisks" && label != "noconstraints" && label != "noknownconstraints";
}
}

std::string ToString(const DeliverableSection section)
{
    switch (section)
    {
    case DeliverableSection::Steps:
        return "steps";
    case DeliverableSection::Constraints:
        return "constraints";
    case DeliverableSection::Risks:
        return "risks";
    case DeliverableSection::AcceptanceCriteria:
        return "acceptanceCriteria";
    }
    return "unknown";
}

bool ValidateDeliverableContract(const DeliverableContract& contract, std::string& error)
{
    std::set<DeliverableSection> sections;
    for (const auto& requirement : contract.requirements)
    {
        if (ToString(requirement.section) == "unknown" || !sections.insert(requirement.section).second ||
            !Text(requirement.instruction, 1024) ||
            (requirement.allowNoneWithReason && requirement.section != DeliverableSection::Risks &&
                requirement.section != DeliverableSection::Constraints))
        {
            error = "Deliverable requirements must be unique, bounded and explicit; only risks or constraints may be absent with a reason.";
            return false;
        }
    }
    if (sections.empty() && contract.requireAllPrerequisites)
    {
        error = "Evidence references require an explicit deliverable contract.";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateDeliverable(const DeliverableContract& contract, const std::string& content,
    const std::vector<ArtifactReference>& actualPrerequisites, std::string& error)
{
    if (!ValidateDeliverableContract(contract, error))
        return false;
    if (contract.requirements.empty())
        return true;
    const auto fail = [&error](const char* reason)
    {
        error = reason;
        return false;
    };
    if (content.size() > 8192 || !utf8::IsValid(content))
        return fail("The analytical artifact exceeds its bounded UTF-8 contract.");
    try
    {
        const auto payload = nlohmann::json::parse(content);
        std::set<std::string> allowed{"summary", "evidence", "prerequisiteEvidence"};
        for (const auto& requirement : contract.requirements)
            allowed.insert(ToString(requirement.section));
        if (!payload.is_object() || payload.size() != allowed.size())
            return fail("The analytical artifact is missing required fields or contains unknown fields.");
        for (const auto& [key, value] : payload.items())
            if (!allowed.contains(key))
                return fail("The analytical artifact contains an unknown field.");
        if (!payload.at("summary").is_string() || !Text(payload.at("summary").get<std::string>(), 2048) ||
            !payload.at("evidence").is_string() || !Text(payload.at("evidence").get<std::string>(), 2048))
            return fail("The analytical summary and evidence must contain bounded text.");
        for (const auto& requirement : contract.requirements)
        {
            const auto& section = payload.at(ToString(requirement.section));
            if (!section.is_object() || section.size() != 2 || !section.contains("items") || !section.contains("noneReason") ||
                !section.at("items").is_array() || !section.at("noneReason").is_string() || section.at("items").size() > 8)
                return fail("A required deliverable section has an invalid structure.");
            const auto reason = section.at("noneReason").get<std::string>();
            if (section.at("items").empty())
            {
                if (!requirement.allowNoneWithReason || !AbsenceRationale(reason))
                    return fail("A required deliverable section has no items or permitted explanation.");
            }
            else
            {
                if (!reason.empty())
                    return fail("A deliverable section cannot report both items and their absence.");
                for (const auto& item : section.at("items"))
                    if (!item.is_string() || !Text(item.get<std::string>(), 1024))
                        return fail("Deliverable items must contain bounded text.");
            }
        }
        const auto& references = payload.at("prerequisiteEvidence");
        if (!references.is_array() || references.size() > actualPrerequisites.size() ||
            (contract.requireAllPrerequisites && references.size() != actualPrerequisites.size()))
            return fail("The deliverable must identify its exact prerequisite evidence.");
        std::set<std::string> seen;
        for (const auto& row : references)
        {
            if (!row.is_object() || row.size() != 4 || !row.at("nodeId").is_string() || !row.at("id").is_string() ||
                !row.at("version").is_number_unsigned() || !row.at("hash").is_string())
                return fail("A prerequisite evidence reference is malformed.");
            const ArtifactReference reference{row.at("nodeId"), row.at("id"), row.at("version"), row.at("hash")};
            if (!seen.insert(reference.nodeId).second || !std::any_of(actualPrerequisites.begin(), actualPrerequisites.end(),
                                                             [&](const auto& actual) { return SameReference(reference, actual); }))
                return fail("A prerequisite evidence reference is duplicate, stale or was not supplied to this attempt.");
        }
    }
    catch (...)
    {
        return fail("The analytical artifact failed its typed JSON contract.");
    }
    error.clear();
    return true;
}
}
