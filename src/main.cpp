#include <cerrno>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>

#include <uhd/version.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/log_add.hpp>

extern "C" {
    #include <sched.h>
}

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
    const Config cfg {argv[1]};

    // block signals that will be handled via sigtimedwait() in main loop
    // before spawning any threads that will inherit the sigmask
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int ret = pthread_sigmask(SIG_BLOCK, &set, nullptr);
    if (ret != 0)
        throw syscall_error{"pthread_sigmask() failed", ret};

    // spawn Logger
    logger = std::make_shared<Logger>("usrp_rxtx", std::move(cfg.to_json()));

    // logger can handle all exceptions from now on
    int exit_code = 0;
    try {
        // set up UHD logging
        uhd::log::set_console_level(uhd::log::off);
        uhd::log::add_logger("asdf", log_uhd);

        logger->log("Initializing ...", Log::INFO);

        // spawn MQTT client
        MqttClient::sptr mqtt = std::make_shared<MqttClient>(logger);

        // register power management quality-of-service (PM QoS) request with
        // kernel. inhibits certain power mangement features that increase
        // latency above specified threshold (e.g., C-states).
        // https://www.kernel.org/doc/html/latest/power/pm_qos_interface.html
        std::ofstream dev_latency {"/dev/cpu_dma_latency", std::ios_base::binary};
        if (!dev_latency)
            throw syscall_error{"error opening /dev/cpu_dma_latency"};
        dev_latency.write((char *) &(cfg.cpu.max_latency_usec), 4);
        dev_latency.flush();
        if (!dev_latency)
            throw syscall_error{"error writing to /dev/cpu_dma_latency"};
        // keep file open until process terminates

        int oldsched = sched_getscheduler(0);
        if (oldsched == -1)
            throw syscall_error{"sched_getscheduler() failed"};
        struct sched_param oldparam;
        if (sched_getparam(0, &oldparam) == -1)
            throw syscall_error{"sched_getparam() failed"};

        // as of UHD version 4.6.0, creating a uhd::usrp::multi_usrp instance
        // spawns multiple threads. with an X310 these include `uhd_ctrl_ep0001`,
        // which handle time-critical network communication. therefore, elevate
        // the main thread/process to real-time priority before instantiating
        // multi_usrp, so its threads will inherit the priority.
        const struct sched_param param = {
            .sched_priority = 99
        };
        if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
            throw syscall_error{"sched_setscheduler() failed"};

        // connect to USRP
        uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(cfg.usrp.args);

        // return to original scheduler settings. the remaining threads are under
        // our control and will set their own priority autonomously.
        if (sched_setscheduler(0, oldsched, &oldparam) == -1)
            throw syscall_error{"sched_setscheduler() failed"};

        // channel selection
        usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t{cfg.rx.subdev});
        usrp->set_tx_subdev_spec(uhd::usrp::subdev_spec_t{cfg.tx.subdev});
        size_t rx_num_channels = usrp->get_rx_num_channels();

        // log USRP hardware and channel configuration
        logger->log_usrp_hardware(usrp);
        logger->log_usrp_channels(usrp);

        // allocate Rx ringbuffers
        // NOTE: allocate 2M hugepages via `echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`
        std::vector<Ringbuf::sptr> rx_ringbufs;
        for (size_t ch = 0; ch < rx_num_channels; ch++) {
            std::filesystem::path desc_path {cfg.shmem.mount_desc};
            std::filesystem::path ring_path {cfg.shmem.mount_ring};
            desc_path /= "usrp_rxtx_" + cfg.usrp.args
                       + "_ch" + std::to_string(ch)
                       + "_desc";
            ring_path /= "usrp_rxtx_" + cfg.usrp.args
                       + "_ch" + std::to_string(ch)
                       + "_ring";

            Ringbuf::sptr ringbuf = std::make_shared<Ringbuf>(
                desc_path, cfg.shmem.size_desc_mib << 20,
                ring_path, cfg.shmem.size_ring_mib << 20
            );
            rx_ringbufs.push_back(std::move(ringbuf));
        }

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

            // first verify that logger is still running
            if (!logger->is_running()) {
                std::cerr << "Logging thread stopped unexpectedly. Logging is broken. Terminating."
                          << std::endl;
                exit_code = 1;
                break;
            }

            if (!mqtt->is_running()) {
                logger->log("MQTT client thread stopped unexpectedly. Terminating.",
                            Log::FATAL);
                exit_code = 1;
                break;
            }

            if (n % 10 == 0) {
                std::string mesg = "Main loop iteration: " + std::to_string(n);
                logger->log(std::move(mesg), Log::TRACE);
            }
            n++;
        }
    } catch (const std::exception& e) {
        exit_code = 1;
        logger->log_exception(std::current_exception());
        logger->log("Exception occurred in main thread. Terminating.", Log::FATAL);
    } catch (...) {
        exit_code = 1;
        logger->log("Unknown exception occurred in main thread. Terminating.", Log::FATAL);
    }

    logger->log_exit(exit_code);
    return exit_code;
} catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << '\n'
              << "Exception occurred in main thread. Terminating."
              << std::endl;
    return 1;
} catch (...) {
    std::cerr << "Unknown exception occurred in main thread. Terminating."
              << std::endl;
    return 1;
}
