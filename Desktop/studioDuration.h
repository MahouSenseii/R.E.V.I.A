#pragma once

#include <QString>
#include <cstdint>

namespace revia::desktop
{
inline QString StudioDuration(const std::uint64_t milliseconds)
{
    return QString::number(static_cast<double>(milliseconds) / 1000.0, 'f', 1) + " s";
}
}
