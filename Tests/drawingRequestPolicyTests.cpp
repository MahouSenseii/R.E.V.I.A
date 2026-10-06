#include "Visual/drawingRequestPolicy.h"

#include <iostream>
#include <stdexcept>
#include <string>

int main()
{
    using revia::visual::DrawingRequestPolicy;
    try
    {
        for (const std::string input : {"Generate an illustration of a fox in snow", "Paint a portrait of a dragon",
                 "Please create a picture of a moonlit forest", "Paint a portrait of a chartreuse dragon", "Draw a fox, don't add text"})
        {
            if (!DrawingRequestPolicy::ShouldDraw(input))
            {
                throw std::runtime_error("Raster request was missed: " + input);
            }
            if (DrawingRequestPolicy::Classify(input) != revia::visual::DrawingIntent::Raster)
            {
                throw std::runtime_error("Art was routed as a diagram: " + input);
            }
        }
        const std::string detailed = "Please generate an illustration of a fox. " + std::string(500, 'a');
        if (DrawingRequestPolicy::Classify(detailed) != revia::visual::DrawingIntent::Raster)
        {
            throw std::runtime_error("A detailed art brief lost its raster request.");
        }
        for (const std::string input : {"draw a diagram of the memory pipeline", "Can you sketch the Resources tab layout?",
                 "mock up a settings screen", "illustrate how the goal runner retries", "what would that look like as a diagram?"})
        {
            if (DrawingRequestPolicy::Classify(input) != revia::visual::DrawingIntent::Diagram)
            {
                throw std::runtime_error("Structured drawing lost its renderer: " + input);
            }
        }
        for (const std::string input :
            {"I draw cats every day", "The redraw fixed it", "Do not generate an image", "Can you explain how to sketch a cat?"})
        {
            if (DrawingRequestPolicy::ShouldDraw(input))
            {
                throw std::runtime_error("Discussion or negation became a request: " + input);
            }
        }
        std::cout << "Natural visual request tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
