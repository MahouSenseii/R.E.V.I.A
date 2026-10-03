#include <studioDuration.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#ifdef REVIA_STUDIO_DURATION_STANDALONE
int main(int argc, char** argv)
{
    int digits = argc > 1 ? std::stoi(argv[1]) : 0;
    if (digits == 0)
    {
        const auto sample = revia::desktop::StudioDuration(1000);
        digits = sample == "1.0 s" ? 1 : sample == "1.00 s" ? 2 : sample == "1.000 s" ? 3 : 0;
    }
    if (digits < 1 || digits > 3)
        return 2;
    struct Case { std::uint64_t milliseconds; std::array<const char*, 3> expected; };
    const std::array cases = {
        Case{0, {"0.0 s", "0.00 s", "0.000 s"}},
        Case{1000, {"1.0 s", "1.00 s", "1.000 s"}},
        Case{1234, {"1.2 s", "1.23 s", "1.234 s"}},
        Case{999, {"1.0 s", "1.00 s", "0.999 s"}},
        Case{65000, {"65.0 s", "65.00 s", "65.000 s"}},
        Case{600123, {"600.1 s", "600.12 s", "600.123 s"}}
    };
    for (const auto& item : cases)
    {
        const auto actual = revia::desktop::StudioDuration(item.milliseconds).toStdString();
        if (actual != item.expected[digits - 1])
        {
            std::cerr << "Duration formatting failed at " << item.milliseconds << ": " << actual << '\n';
            return 1;
        }
    }
    std::cout << "6 duration cases passed at " << digits << " decimal places.\n";
    return 0;
}
#endif
