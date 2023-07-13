#include "test.hpp"

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    if (false)
        throw test_error{"unit test failure"};

    return 0;
}
