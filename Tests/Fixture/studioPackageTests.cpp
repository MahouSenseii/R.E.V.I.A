#include <exception>
#include <iostream>

void REVIA_STUDIO_TEST_SUITE();

int main()
{
    try
    {
        REVIA_STUDIO_TEST_SUITE();
        std::cout << "Studio package checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Studio package failure: " << error.what() << '\n';
        return 1;
    }
}
