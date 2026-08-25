#pragma once

#include <string>
#include <string_view>

namespace revia::actions::internet
{

inline constexpr std::string_view VisibleBrowserBackend = "visible_browser";
inline constexpr std::string_view DuckDuckGoApiBackend = "duckduckgo_api";
inline constexpr std::string_view WikipediaApiBackend = "wikipedia_api";

inline std::string BackendDisplayName(const std::string_view backend)
{
    if (backend == VisibleBrowserBackend) return "Visible browser";
    if (backend == DuckDuckGoApiBackend) return "DuckDuckGo API";
    if (backend == WikipediaApiBackend) return "Wikipedia API";
    return backend.empty() ? "Internet backend" : std::string(backend);
}

} // namespace revia::actions::internet
