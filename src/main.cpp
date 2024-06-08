#include <cerrno>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>

#include <uhd/version.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/log_add.hpp>

#include "config.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "mqtt.hpp"
#include "ringbuf.hpp"

// warn about unsupported UHD versions
#if UHD_VERSION < 4030000
    #warning compatibility with UHD versions < 4.3.0 is not verified
#endif

// global Logger::sptr required by log_uhd and exception handlers
static Logger::sptr logger;

// global function enqueueing UHD log messages into our Logger
static void log_uhd(const uhd::log::logging_info& uhd_logging_info)
{
    if (logger) {;
        logger->log_uhd(uhd_logging_info);
    } else {
        return;
    }
}

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
    if (ret != 0)
        throw syscall_error{"pthread_sigmask() failed", ret};

    // spawn Logger and set up UHD logging
    logger = std::make_shared<Logger>("usrp_rxtx", std::move(cfg.to_json()));
    uhd::log::set_console_level(uhd::log::off);
    uhd::log::add_logger("asdf", log_uhd);

    logger->log("Initializing ...", Log::INFO);

    // spawn MQTT client
    MqttClient::sptr mqtt = std::make_shared<MqttClient>(logger);

    // allocate ringbuffer
    // NOTE: allocate 2M hugepages via `echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`
    Ringbuf ring{cfg.shmem.mount_desc + "/usrp_rxtx_desc", cfg.shmem.size_desc_mib << 20,
                 cfg.shmem.mount_ring + "/usrp_rxtx_ring", cfg.shmem.size_ring_mib << 20};

    logger->log("Initialization succeeded.");

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
            logger->log("Received SIGINT or SIGTERM. Exiting gracefully.");
            break;
        }

        if (!logger->is_running()) {
            std::cerr << "Logging thread stopped unexpectedly. Logging is broken. Terminating."
                      << std::endl;
            return 1;
        }

        if (!mqtt->is_running()) {
            logger->log("MQTT client thread stopped unexpectedly. Terminating.",
                        Log::FATAL);
            logger->log_exit(1);
            return 1;
        }

        std::string mesg = "Main loop iteration: " + std::to_string(n++);
        logger->log(std::move(mesg), Log::TRACE);
    }

    logger->log_exit(0);
    return 0;
} catch (const std::exception& e) {
    if (logger) {
        logger->log_exception(std::current_exception());
        logger->log("Exception occurred in main thread. Terminating.", Log::FATAL);
    } else {
        std::cerr << "Exception: " << e.what() << '\n'
                  << "Exception occurred in main thread. Terminating." << std::endl;
    }

    return 1;
} catch (...) {
    if (logger) {
        logger->log_exception(std::current_exception());
        logger->log("Terminating after exception occurred in main thread.", Log::FATAL);
    } else {
        std::cerr << "Unknown exception occurred in main thread. Terminating." << std::endl;
    }

    return 1;
}
