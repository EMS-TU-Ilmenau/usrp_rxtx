#include "test.hpp"
#include "../src/config.hpp"

#include <iostream>

int main(int argc, char *argv[])
{
    if (argc != 2)
        throw test_error{"usage: " + std::string(argv[0]) + " path/to/config.cfg"};

    const Config cfg{argv[1]};
    cfg.to_json().dump(&std::cout);

    return 0;
}
