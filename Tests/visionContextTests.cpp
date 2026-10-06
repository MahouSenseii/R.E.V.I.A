#include "testSupport.h"

#include "Computer/legacyLlmPolicy.h"
#include "Planning/goalPlanner.h"

#include <nlohmann/json.hpp>

void RunVisionContextTests()
{
    using namespace revia;
    computer::ComputerTaskContext context;
    context.observation.screen.succeeded = true;
    context.observation.visualDescription = std::string(5000, 'v');
    const auto formatted = nlohmann::json::parse(computer::FormatLegacyContext(context));
    const auto& observation = formatted.at("observation");
    tests::Check(observation.contains("untrusted_visual_description"), "The admitted vision read was lost before planning.");
    tests::Check(observation.at("untrusted_visual_description").get<std::string>().size() <= 4096,
        "The vision description escaped its prompt bound.");
    context.observation.withheld = true;
    tests::Check(!nlohmann::json::parse(computer::FormatLegacyContext(context)).at("observation").contains("untrusted_visual_description"),
        "A withheld observation leaked its visual description.");
    context.scope.image.enabled = true;
    const auto uncheckable = nlohmann::json::parse(planning::GoalPlanner::NextStepSchema(computer::FormatLegacyContext(context)));
    tests::Check(uncheckable.dump().find("generate_image") == std::string::npos,
        "The operator advertised an image step without any admitted read-only verification action.");
    context.scope.approvedRoots = {std::filesystem::temp_directory_path()};
    const auto schema = nlohmann::json::parse(planning::GoalPlanner::NextStepSchema(computer::FormatLegacyContext(context)));
    bool foundImage = false;
    for (const auto& decision : schema.at("oneOf"))
    {
        if (decision.at("properties").at("decision").at("const") != "act")
            continue;
        const auto& variants = decision.at("properties").at("step").at("properties").at("action").at("oneOf");
        for (const auto& action : variants)
        {
            const auto& properties = action.at("properties");
            if (properties.at("action").at("const") != "generate_image")
                continue;
            foundImage = true;
            tests::Check(properties.contains("prompt") && !properties.contains("source") && !properties.contains("destination") &&
                             action.at("required") == nlohmann::json::array({"action", "prompt"}) &&
                             action.at("additionalProperties") == false,
                "The admitted image tool used filesystem fields instead of its exact prompt contract.");
        }
    }
    tests::Check(foundImage, "The admitted image tool disappeared despite an available read-only artifact check.");
}
