#pragma once

#include <string>

struct commandOutput
{
    bool bWasCommand = false;
    bool bShouldExit = false;
    bool bSuccess = true;

    std::string output;
    std::string reason;
};
