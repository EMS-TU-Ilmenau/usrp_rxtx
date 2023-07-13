#include <cerrno>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>

#include <uhd/version.hpp>
#include <uhd/usrp/multi_usrp.hpp>

#include "config.hpp"
#include "error.hpp"

// warn about unsupported UHD versions
#if UHD_VERSION < 4030000
    #warning compatibility with UHD versions < 4.3.0 is not verified
#endif

int main(int argc, char *argv[])
try {
    // read config file
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " config.cfg" << std::endl;
        return 1;
    }
    const Config cfg{argv[1]};

    // block signals that will be handled via sigtimedwait() in main loop
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int ret = pthread_sigmask(SIG_BLOCK, &set, nullptr);
    if (ret == -1)
        throw syscall_error{"pthread_sigmask() failed", ret};

    // run until interrupted
    int n = 0;
    while (true) {
        // signal handling with timeout (break loop on SIGINT, or SIGTERM,
        // but ignore SIGHUP)
        const struct timespec timeout = { .tv_sec = 0, .tv_nsec = 100'000'000UL };
        int ret = sigtimedwait(&set, nullptr, &timeout);
        if (ret == -1 && errno != EAGAIN) {
            throw syscall_error{"sigtimedwait() failed"};
        } else if (ret == SIGINT || ret == SIGTERM) {
            break;
        }

        std::cerr << "Main loop iteration: " << n++ << std::endl;
    }

    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
} catch (...) {
    std::cerr << "Unknown exception" << std::endl;
    return 1;
}
