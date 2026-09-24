#pragma once

#include "Vision/visionActionTypes.h"

#include <string>

namespace revia::vision
{

class VisionActionParser
{
public:
    [[nodiscard]] VisionActionParseResult Parse(const std::string& response) const;
};

} // namespace revia::vision
