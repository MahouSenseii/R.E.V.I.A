#include "Autonomy/activityExecution.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace revia::autonomy
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
            return std::isspace(character) == 0;
        };
        const auto first = std::find_if(value.begin(), value.end(), notSpace);
        if (first == value.end()) return {};
        const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
        return std::string(first, last);
    }

    // Trailing punctuation carries no meaning for a search and makes an exact-match
    // check against a delegation phrase fail for no reason ("what you want !").
    std::string StripEdgePunctuation(std::string value)
    {
        const auto isEdge = [](const unsigned char character)
        {
            return std::ispunct(character) != 0 || std::isspace(character) != 0;
        };
        while (!value.empty() && isEdge(static_cast<unsigned char>(value.back())))
        {
            value.pop_back();
        }
        std::size_t start = 0;
        while (start < value.size() &&
            isEdge(static_cast<unsigned char>(value[start])))
        {
            ++start;
        }
        return value.substr(start);
    }

    // The imperative that introduces a research request. Removing it is what turns a
    // command into the subject it was a command about.
    constexpr std::array ResearchVerbs{
        std::string_view{"look up"},
        std::string_view{"look into"},
        std::string_view{"search for"},
        std::string_view{"search"},
        std::string_view{"find out about"},
        std::string_view{"find out"},
        std::string_view{"read about"},
        std::string_view{"read up on"},
        std::string_view{"research"},
        std::string_view{"google"},
        std::string_view{"investigate"},
        std::string_view{"tell me about"},
        std::string_view{"learn about"},
    };

    // Phrases that hand the choice back rather than naming a subject. Matched only
    // against what is left after the verb, so "look up what you want" refuses while
    // "look up what you want to eat in Osaka" does not.
    constexpr std::array Delegations{
        std::string_view{"what you want"},
        std::string_view{"whatever you want"},
        std::string_view{"whatever"},
        std::string_view{"anything"},
        std::string_view{"something"},
        std::string_view{"anything you want"},
        std::string_view{"something interesting"},
        std::string_view{"you decide"},
        std::string_view{"your choice"},
        std::string_view{"up to you"},
        std::string_view{"whatever you like"},
        std::string_view{"whatever you find interesting"},
        std::string_view{"stuff"},
        std::string_view{"things"},
        std::string_view{"it"},
        std::string_view{"that"},
        std::string_view{"this"},
    };

    // Words that cannot carry a subject on their own.
    constexpr std::array Stopwords{
        std::string_view{"the"}, std::string_view{"a"}, std::string_view{"an"},
        std::string_view{"and"}, std::string_view{"or"}, std::string_view{"of"},
        std::string_view{"for"}, std::string_view{"to"}, std::string_view{"in"},
        std::string_view{"on"}, std::string_view{"about"}, std::string_view{"with"},
        std::string_view{"you"}, std::string_view{"me"}, std::string_view{"my"},
        std::string_view{"your"}, std::string_view{"want"}, std::string_view{"like"},
        std::string_view{"please"}, std::string_view{"some"}, std::string_view{"any"},
        std::string_view{"what"}, std::string_view{"whatever"},
        std::string_view{"it"}, std::string_view{"that"}, std::string_view{"this"},
    };

    bool IsStopword(const std::string& word)
    {
        return std::find(Stopwords.begin(), Stopwords.end(), word) != Stopwords.end();
    }

    std::vector<std::string> Words(const std::string& value)
    {
        std::vector<std::string> words;
        std::string current;
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) != 0)
            {
                current.push_back(static_cast<char>(std::tolower(character)));
                continue;
            }
            if (!current.empty())
            {
                words.push_back(current);
                current.clear();
            }
        }
        if (!current.empty()) words.push_back(current);
        return words;
    }
}

std::optional<Drive> DriveSatisfiedBy(const ActivityType type)
{
    switch (type)
    {
        case ActivityType::Think: return Drive::Learning;
        case ActivityType::Observe: return Drive::Exploration;
        case ActivityType::Research: return Drive::Curiosity;
        case ActivityType::ContinueGoal: return Drive::UnfinishedGoal;
        case ActivityType::OrganizeMemory: return Drive::Learning;
        case ActivityType::Create: return Drive::Creativity;
        case ActivityType::Speak: return Drive::Social;
        // Deliberately nothing. Declining to act is not a way of getting what you
        // wanted, and spending a drive for it would make a quiet Revia steadily less
        // motivated the longer she stayed quiet.
        case ActivityType::Nothing: break;
    }
    return std::nullopt;
}

ResearchTopicVerdict ResolveResearchTopic(const std::string& candidate)
{
    ResearchTopicVerdict verdict;
    const std::string trimmed = Trim(candidate);
    if (trimmed.empty())
    {
        verdict.refusal = "No research topic was supplied.";
        return verdict;
    }

    // Strip one leading research verb, longest first so "find out about" is not
    // shortened to "find out" and left with a dangling "about".
    std::string body = trimmed;
    std::string lowered = Lower(body);
    std::vector<std::string_view> verbs(ResearchVerbs.begin(), ResearchVerbs.end());
    std::sort(verbs.begin(), verbs.end(),
        [](const std::string_view left, const std::string_view right)
        {
            return left.size() > right.size();
        });
    for (const std::string_view verb : verbs)
    {
        if (lowered.rfind(verb, 0) == 0)
        {
            body = Trim(body.substr(verb.size()));
            lowered = Lower(body);
            break;
        }
    }

    const std::string core = StripEdgePunctuation(body);
    const std::string loweredCore = Lower(core);
    if (core.empty())
    {
        verdict.delegated = true;
        verdict.refusal =
            "\"" + trimmed + "\" asks for research without naming a subject.";
        return verdict;
    }
    if (std::find(Delegations.begin(), Delegations.end(), loweredCore) !=
        Delegations.end())
    {
        verdict.delegated = true;
        verdict.refusal = "\"" + trimmed +
            "\" hands the choice of topic back rather than naming one. Searching the "
            "request itself returns results about the words in it.";
        return verdict;
    }

    const std::vector<std::string> words = Words(core);
    const auto contentWords = std::count_if(words.begin(), words.end(),
        [](const std::string& word)
        {
            return word.size() >= 3 && !IsStopword(word);
        });
    if (contentWords == 0)
    {
        verdict.delegated = true;
        verdict.refusal = "\"" + trimmed +
            "\" contains no subject to research.";
        return verdict;
    }

    verdict.usable = true;
    verdict.topic = core;
    return verdict;
}

ResearchTopicVerdict ChooseResearchTopic(
    const std::vector<std::string>& candidatesInPriorityOrder)
{
    ResearchTopicVerdict lastRefusal;
    lastRefusal.refusal = "Nothing Revia is currently curious about names a subject.";
    for (const std::string& candidate : candidatesInPriorityOrder)
    {
        ResearchTopicVerdict verdict = ResolveResearchTopic(candidate);
        if (verdict.usable) return verdict;
        if (!candidate.empty()) lastRefusal = verdict;
    }
    return lastRefusal;
}

std::string WorkspaceArtifactName(
    const std::string& title,
    const std::string& extension)
{
    std::string name;
    bool separator = false;
    for (const unsigned char character : title)
    {
        if (std::isalnum(character) != 0)
        {
            name.push_back(static_cast<char>(std::tolower(character)));
            separator = false;
            continue;
        }
        if (!name.empty() && !separator)
        {
            name.push_back('-');
            separator = true;
        }
    }
    while (!name.empty() && name.back() == '-') name.pop_back();
    if (name.empty()) name = "note";
    // Bounded so a long model-written title cannot produce a path the filesystem
    // refuses, and so nothing here can reach outside the workspace by construction:
    // only lowercase alphanumerics and single hyphens survive the loop above.
    if (name.size() > 60) name.resize(60);
    while (!name.empty() && name.back() == '-') name.pop_back();
    return name + extension;
}

std::string DescribeLearnedFindingArtifact(
    const agents::LearnedFindingResult result,
    const std::string& kept,
    const std::string& pending)
{
    switch (result)
    {
        case agents::LearnedFindingResult::SavedWithoutEmbedding:
        case agents::LearnedFindingResult::AlreadyExists:
            return kept;
        case agents::LearnedFindingResult::Queued:
            return pending;
        case agents::LearnedFindingResult::Failed:
            return {};
    }
    return {};
}

} // namespace revia::autonomy
