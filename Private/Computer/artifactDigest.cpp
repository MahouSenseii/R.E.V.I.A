#include "artifactDigest.h"

#include "Audit/contentDigest.h"
#include <cstdio>
#include <sstream>

namespace revia::computer
{

std::string Sha256Hex(const std::string& input)
{
    return audit::ContentDigest(input);
}

std::string BehaviourDigestInput(const std::uint32_t featureVersion, const std::vector<std::string>& featureNames,
    const std::vector<double>& weights, const double abstainBelow, const std::vector<std::string>& applications,
    const std::vector<std::string>& intents)
{
    // Newline-separated and section-labelled, so that moving a value from one list to
    // another changes the digest. A bare concatenation would let "ab" + "c" and "a" +
    // "bc" hash the same.
    const auto number = [](const double value)
    {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.17g", value);
        return std::string(buffer);
    };

    std::ostringstream stream;
    stream << "feature_version=" << featureVersion << '\n';
    stream << "features\n";
    for (const std::string& name : featureNames) stream << name << '\n';
    stream << "weights\n";
    for (const double weight : weights) stream << number(weight) << '\n';
    stream << "abstain_below=" << number(abstainBelow) << '\n';
    stream << "applications\n";
    for (const std::string& application : applications) stream << application << '\n';
    stream << "intents\n";
    for (const std::string& intent : intents) stream << intent << '\n';
    return stream.str();
}

} // namespace revia::computer
