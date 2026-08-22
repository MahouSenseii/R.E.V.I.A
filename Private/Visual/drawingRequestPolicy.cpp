#include "Visual/drawingRequestPolicy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace revia::visual
{

namespace
{

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string Trim(std::string value)
{
    const auto notSpace = [](const unsigned char character)
    {
        return !std::isspace(character);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

// Verbs that only ever mean "produce a picture".
//
// "chart" and "diagram" are deliberately absent. They are nouns far more often than
// verbs -- "the chart showed a drop last quarter" is a sentence about data, not a request
// to draw one -- so they live in the noun list below and need a request phrase alongside
// them. A recognizer that fires on any mention of a picture would turn ordinary
// conversation into unwanted drawings, which is worse than having no shortcut at all.
constexpr std::array DrawVerbs = {
    std::string_view("draw"), std::string_view("sketch"),
    std::string_view("mock up"), std::string_view("mockup"),
    std::string_view("wireframe"), std::string_view("illustrate"),
    std::string_view("visualise"), std::string_view("visualize")
};

// Nouns that make an otherwise ambiguous "show me" a request for a picture.
constexpr std::array VisualNouns = {
    std::string_view("diagram"), std::string_view("mockup"), std::string_view("mock-up"),
    std::string_view("wireframe"), std::string_view("layout"), std::string_view("sketch"),
    std::string_view("flowchart"), std::string_view("flow chart"),
    std::string_view("chart"), std::string_view("drawing"), std::string_view("picture")
};

bool Contains(const std::string& haystack, const std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

bool DrawingRequestPolicy::ShouldDraw(const std::string& input)
{
    const std::string lowered = Lower(Trim(input));
    // A drawing request is a sentence, not an essay. The ceiling keeps a long message
    // that happens to contain "chart" from turning into a drawing nobody asked for.
    if (lowered.empty() || lowered.size() > 400)
    {
        return false;
    }
    // Never hijack a command.
    if (lowered.front() == '/')
    {
        return false;
    }
    // Asking what something looks like, or discussing drawings, is not asking for one.
    if (Contains(lowered, "how do you draw") || Contains(lowered, "can you draw?") ||
        Contains(lowered, "don't draw") || Contains(lowered, "do not draw") ||
        Contains(lowered, "instead of a diagram"))
    {
        return false;
    }

    const bool hasVerb = std::any_of(DrawVerbs.begin(), DrawVerbs.end(),
        [&lowered](const std::string_view verb) { return Contains(lowered, verb); });
    if (hasVerb)
    {
        return true;
    }

    // "show me the layout", "what would that look like as a flowchart".
    const bool hasNoun = std::any_of(VisualNouns.begin(), VisualNouns.end(),
        [&lowered](const std::string_view noun) { return Contains(lowered, noun); });
    if (!hasNoun)
    {
        return false;
    }
    return Contains(lowered, "show me") || Contains(lowered, "look like") ||
        Contains(lowered, "give me a") || Contains(lowered, "make me a") ||
        Contains(lowered, "can i see");
}

std::string DrawingRequestPolicy::ExtractSubject(const std::string& input)
{
    std::string subject = Trim(input);
    const std::string lowered = Lower(subject);

    // Strip a leading request framing so the drawing prompt receives the subject rather
    // than the politeness around it.
    constexpr std::array Prefixes = {
        std::string_view("could you please "), std::string_view("could you "),
        std::string_view("can you please "), std::string_view("can you "),
        std::string_view("would you "), std::string_view("please "),
        std::string_view("i'd like you to "), std::string_view("i want you to "),
        std::string_view("let's see ")
    };
    for (const std::string_view prefix : Prefixes)
    {
        if (lowered.rfind(prefix, 0) == 0)
        {
            subject = Trim(subject.substr(prefix.size()));
            break;
        }
    }
    while (!subject.empty() && (subject.back() == '.' || subject.back() == '?' ||
        subject.back() == '!'))
    {
        subject.pop_back();
    }
    return subject.empty() ? input : subject;
}

} // namespace revia::visual
