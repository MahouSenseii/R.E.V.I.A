#pragma once

#include <string>
#include <vector>

struct embeddingOutput
{
    bool bSuccess = false;
    std::vector<float> values;
    std::string model;
    std::string reason;
    double elapsedMilliseconds = 0.0;
};
