#include "Core/logger.h"
#include <iostream>
#include "Library/enumLibrary.h"
using namespace std;

logger::logger() = default;

logger::~logger() = default;

void logger::Log(const std::string &message) {
    cout << "[Log] " + message << endl;
}

void logger::Warning(const std::string &reason, const std::source_location &location) {
    cout <<"[Warning] "
        << "File: " << location.file_name()
        << " Line: " << location.line()
        << " Function: " << location.function_name()
        << " Reason: " << reason
        << "\n";
}

void logger::Error(const std::string &reason, const std::source_location &location) {
    cout <<"[Error] "
    << "File: " << location.file_name()
    << " Line: " << location.line()
    << " Function: " << location.function_name()
    << " Reason: " << reason
    << "\n";
}

bool logger::Check(bool bCondition, logSeverity severity, const std::string &reason, const std::source_location &location) {
    if (!bCondition)
    {
        if (severity == logSeverity::Error)
        {
            Error(reason, location);
        }
        else
        {
           Warning(reason, location);

        }
        return false;
    }
    return true;
}


