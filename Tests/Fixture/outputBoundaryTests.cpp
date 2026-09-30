#include <exception>
#include <iostream>

void RunReviewDeliveryTests();
void RunUtf8GuidanceTests();
void RunUtf8PersistenceTests();

int main()
{
    try
    {
        RunReviewDeliveryTests();
        RunUtf8GuidanceTests();
        RunUtf8PersistenceTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Output boundary test failure: " << error.what() << '\n';
        return 1;
    }
}
