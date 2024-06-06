#include "test.hpp"
#include "../src/ringbuf.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

extern "C" {
    #include <unistd.h>
}

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    uint64_t epoch_nsec = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    std::filesystem::path desc_path {"/tmp"};
    std::filesystem::path ring_path {"/tmp"};
    std::string file_prefix {"usrp_rxtx_test_ringbuf_"
                            + std::to_string(getpid()) + "_"
                            + std::to_string(epoch_nsec)};
    desc_path /= file_prefix + "_desc";
    ring_path /= file_prefix + "_ring";

    Ringbuf ring {desc_path, 4096, ring_path, 1024 * 1024};

    return 0;
}
