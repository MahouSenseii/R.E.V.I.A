#include "Visual/videoRequestPolicy.h"

#include <iostream>
#include <utility>

int main()
{
    const std::pair<const char*, bool> cases[] = {{"Generate a short video of a red paper boat.", true},
        {"Create an anime animation of a cat waving.", true}, {"Please make a short clip of rain.", true},
        {"Animate the image you generated.", true}, {"Can you turn this image into a video?", true}, {"Make this image move gently.", true},
        {"/video a boat on a pond", true}, {"/animate that image", true}, {"Inspect that clip at 00:12.", false},
        {"Compare the movement before and after the transition.", false}, {"How can I make a video?", false},
        {"Can you explain how to make a video?", false}, {"What does the video show?", false},
        {"Read the words 'generate a video' in the image.", false}, {"Draw an actual anime picture.", false},
        {"Make the image warmer.", false}, {"I saw an animation today.", false}, {"/video status", false}, {"/video cancel", false},
        {"", false}};
    for (const auto& [input, expected] : cases)
        if (revia::visual::IsVideoGenerationRequest(input) != expected)
        {
            std::cerr << "Video generation routing failed: " << input << '\n';
            return 1;
        }
    if (revia::visual::VideoGenerationPrompt("/video a boat on a pond") != "a boat on a pond")
        return 1;
    std::cout << "Explicit video creation and ordinary clip review remain distinct.\n";
    return 0;
}
