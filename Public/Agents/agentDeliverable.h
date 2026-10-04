#pragma once

#include <string>
#include <vector>

namespace revia::agents
{

struct ArtifactReference;

enum class DeliverableSection
{
    Steps,
    Constraints,
    Risks,
    AcceptanceCriteria
};

struct DeliverableRequirement
{
    DeliverableSection section = DeliverableSection::Steps;
    std::string instruction;
    bool allowNoneWithReason = false;
};

struct DeliverableContract
{
    std::vector<DeliverableRequirement> requirements;
    bool requireAllPrerequisites = false;
};

[[nodiscard]] std::string ToString(DeliverableSection section);
[[nodiscard]] bool ValidateDeliverableContract(const DeliverableContract& contract, std::string& error);
// Completeness and evidence identity are native checks; relevance and truth still require review.
[[nodiscard]] bool ValidateDeliverable(const DeliverableContract& contract, const std::string& content,
    const std::vector<ArtifactReference>& actualPrerequisites, std::string& error);

}
