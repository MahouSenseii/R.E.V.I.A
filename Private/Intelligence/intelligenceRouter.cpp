#include "Intelligence/intelligenceRouter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace revia::intelligence
{
namespace
{
std::string Normalize(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n.!?");
    return value.substr(first, last - first + 1);
}

template <std::size_t Size>
bool ContainsAny(
    const std::string& value,
    const std::array<std::string_view, Size>& signals)
{
    return std::any_of(signals.begin(), signals.end(), [&](const std::string_view signal)
    {
        return value.find(signal) != std::string::npos;
    });
}

IntelligenceDecision Decision(
    const IntelligenceTier tier,
    const ReasoningMode mode,
    std::string model,
    std::string reason,
    const float confidence)
{
    IntelligenceDecision result;
    result.requestedTier = tier;
    result.selectedTier = tier;
    result.mode = mode;
    result.selectedModel = std::move(model);
    result.reason = std::move(reason);
    result.confidence = confidence;
    return result;
}
}

IntelligenceDecision IntelligenceRouter::Route(
    const std::string& input,
    const RoutingContext& context) const
{
    const std::string text = Normalize(input);

    static constexpr std::array ReflexSignals = {
        std::string_view{"revia"}, std::string_view{"stop"},
        std::string_view{"cancel"}, std::string_view{"wait"},
        std::string_view{"pause"}, std::string_view{"quiet"},
        std::string_view{"shut up"}, std::string_view{"never mind"},
        std::string_view{"nevermind"}
    };
    if (std::find(ReflexSignals.begin(), ReflexSignals.end(), text) != ReflexSignals.end())
    {
        return Decision(
            IntelligenceTier::Reflex, ReasoningMode::Fast, "C++ ReflexRouter",
            "An exact immediate-interruption or attention phrase needs no model.", 0.99F);
    }

    if (context.visionRequired)
    {
        const bool expert = context.expertVisionPreferred ||
            context.suppliedFileCount >= 3 || context.previousUncertainty;
        return Decision(
            expert ? IntelligenceTier::ExpertVision : IntelligenceTier::Vision,
            expert ? ReasoningMode::Deep : ReasoningMode::Fast,
            expert
                ? "Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf"
                : "Qwen3.5-4B-Q4_K_M.gguf",
            expert
                ? "Difficult or multi-image visual reasoning needs the Expert projector."
                : "The request depends on visual evidence from the normal desktop context.",
            expert ? 0.88F : 0.9F);
    }

    static constexpr std::array ExpertSignals = {
        std::string_view{"deadlock"}, std::string_view{"race condition"},
        std::string_view{"use-after-free"}, std::string_view{"undefined behavior"},
        std::string_view{"system architecture"}, std::string_view{"across these files"},
        std::string_view{"multi-file"}, std::string_view{"root cause"},
        std::string_view{"unreal engine"}, std::string_view{"blueprint graph"},
        std::string_view{"think hard"}, std::string_view{"deep analysis"},
        std::string_view{"research synthesis"}, std::string_view{"threat model"},
        std::string_view{"ownership across"}, std::string_view{"shutdown invalidates"}
    };
    const bool looksLikeStackTrace = text.find("exception") != std::string::npos ||
        text.find("stack trace") != std::string::npos ||
        (text.find(" at ") != std::string::npos && text.find("line ") != std::string::npos);
    if (context.suppliedFileCount >= 3 || context.previousUncertainty ||
        looksLikeStackTrace || ContainsAny(text, ExpertSignals))
    {
        return Decision(
            IntelligenceTier::Expert, ReasoningMode::Deep,
            "Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf",
            context.suppliedFileCount >= 3
                ? "The request spans several supplied files or components."
                : "The request contains high-complexity technical or deep-analysis signals.",
            0.87F);
    }

    static constexpr std::array MainSignals = {
        std::string_view{"explain"}, std::string_view{"implement"},
        std::string_view{"debug"}, std::string_view{"error"},
        std::string_view{"crash"}, std::string_view{"code"},
        std::string_view{"function"}, std::string_view{"pointer"},
        std::string_view{"plan"}, std::string_view{"summarize"},
        std::string_view{"remember"}, std::string_view{"project"},
        std::string_view{"compare"}, std::string_view{"why does"},
        std::string_view{"how do"}, std::string_view{"look up"},
        std::string_view{"search"}, std::string_view{"latest"}
    };
    if (context.explicitResearch || context.toolUseRequested ||
        context.recentContextCharacters > 6000 || ContainsAny(text, MainSignals))
    {
        const bool deep = context.explicitResearch || looksLikeStackTrace;
        return Decision(
            IntelligenceTier::Main,
            deep ? ReasoningMode::Deep : ReasoningMode::Fast,
            "Qwen3.5-4B-Q4_K_M.gguf",
            context.explicitResearch
                ? "Research requires grounded synthesis by the Main brain."
                : "The turn needs normal explanation, planning, code, or contextual nuance.",
            0.84F);
    }

    static constexpr std::array FastExact = {
        std::string_view{"hi"}, std::string_view{"hello"},
        std::string_view{"hey"}, std::string_view{"thanks"},
        std::string_view{"thank you"}, std::string_view{"okay"},
        std::string_view{"ok"}, std::string_view{"cool"},
        std::string_view{"really"}, std::string_view{"why"},
        std::string_view{"what's up"}, std::string_view{"whats up"},
        std::string_view{"that's funny"}, std::string_view{"thats funny"}
    };
    static constexpr std::array FastSocialSignals = {
        std::string_view{"do you like"}, std::string_view{"how are you"},
        std::string_view{"you're annoying"}, std::string_view{"you are annoying"},
        std::string_view{"tell me a joke"}, std::string_view{"what are you doing"}
    };
    if (std::find(FastExact.begin(), FastExact.end(), text) != FastExact.end() ||
        ContainsAny(text, FastSocialSignals))
    {
        return Decision(
            IntelligenceTier::Fast, ReasoningMode::Fast,
            "Qwen3.5-0.8B-Q4_K_M.gguf",
            "A simple social or conversational turn does not need deeper inference.", 0.94F);
    }

    return Decision(
        IntelligenceTier::Main, ReasoningMode::Fast,
        "Qwen3.5-4B-Q4_K_M.gguf",
        "Ambiguous conversation defaults to the balanced Main brain to preserve nuance.",
        0.62F);
}

} // namespace revia::intelligence
