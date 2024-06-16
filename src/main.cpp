#include <cerrno>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>

extern "C" {
    #include <sched.h>
}

#include <uhd/version.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/log_add.hpp>

#include "config.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "mqtt.hpp"
#include "ringbuf.hpp"
#include "rx.hpp"
#include "sync.hpp"
#include "tx.hpp"
#include "wr.hpp"

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
    const Config cfg {argv[1]};

    // block signals that will be handled via sigtimedwait() in main loop
    // before spawning any threads that will inherit the sigmask
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    int ret = pthread_sigmask(SIG_BLOCK, &set, nullptr);
    if (ret != 0)
        throw syscall_error{"pthread_sigmask() failed", ret};

    // spawn Logger
    Logger::sptr logger = std::make_shared<Logger>(
        "usrp_rxtx_" + cfg.usrp.args, std::move(cfg.to_json()));
    logger->log("Initializing ...", Log::INFO);

    // set up UHD logging
    uhd::log::add_logger("usrp_rxtx",
        [logger](const uhd::log::logging_info& uhd_logging_info) {
            logger->log_uhd(uhd_logging_info);
        }
    );
    uhd::log::set_console_level(uhd::log::off);

    // logger can handle all exceptions from now on
    int exit_code = 0;
    try {
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

        // store current scheduler settings before changing it below
        int oldsched = sched_getscheduler(0);
        if (oldsched == -1)
            throw syscall_error{"sched_getscheduler() failed"};
        struct sched_param oldparam;
        if (sched_getparam(0, &oldparam) == -1)
            throw syscall_error{"sched_getparam() failed"};

        // as of UHD version 4.6.0, creating a uhd::usrp::multi_usrp instance
        // spawns multiple threads. with an X310 these include `uhd_ctrl_ep0001`,
        // which handles time-critical network communication. therefore, elevate
        // the main thread/process to real-time priority before instantiating
        // multi_usrp, so threads spawned by uhd::usrp::multi_usrp::make will
        // inherit real-time scheduling.
        const struct sched_param param = {
            .sched_priority = 99
        };
        if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
            throw syscall_error{"sched_setscheduler() failed"};

        // connect to USRP
        uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(cfg.usrp.args);

        // restore original scheduler settings. the remaining threads are under
        // our control and will set their own priority autonomously.
        if (sched_setscheduler(0, oldsched, &oldparam) == -1)
            throw syscall_error{"sched_setscheduler() failed"};

        // select subdevice and sample rate first
        // NOTE: setting sample rate may change master clock rate (e.g., on B205)
        if (!cfg.rx.subdev.empty()) {
            usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t{cfg.rx.subdev});
            usrp->set_rx_rate(cfg.rx.rate);
        }
        if (!cfg.tx.subdev.empty()) {
            usrp->set_tx_subdev_spec(uhd::usrp::subdev_spec_t{cfg.tx.subdev});
            usrp->set_tx_rate(cfg.tx.rate);
        }

        // synchronize **after** setting the master clock rate (MCR) as any
        // change to MCR will cause jump in device time
        Sync sync {usrp, logger, cfg};
        if (cfg.usrp.sync == "10mhz") {
            sync.sync_10mhz();
        } else if (cfg.usrp.sync == "1pps") {
            sync.sync_1pps();
        } else if (cfg.usrp.sync == "10mhz+1pps") {
            sync.sync_10mhz();
            sync.sync_1pps();
        } else if (!cfg.usrp.sync.empty()) {
            throw generic_error{"unknown usrp.sync setting: " + cfg.usrp.sync};
        }

        // tune Rx and Tx frontends
        // TODO: implement synchronous tuning
        // https://files.ettus.com/manual/page_sync.html#sync_phase
        // https://files.ettus.com/manual/page_usrp_x3x0.html#x3x0_misc_timed_cmds_lockup
        if (!cfg.rx.subdev.empty()) {
            // prepare tune request
            uhd::tune_request_t tune_req {cfg.rx.freq_rf};
            tune_req.args["mode_n"] = "integer";
            tune_req.target_freq = cfg.rx.freq_rf;
            tune_req.rf_freq = cfg.rx.freq_rf;
            tune_req.dsp_freq = cfg.rx.freq_dsp;
            tune_req.rf_freq_policy  = uhd::tune_request_t::POLICY_MANUAL;
            tune_req.dsp_freq_policy = uhd::tune_request_t::POLICY_MANUAL;

            for (size_t ch = 0; ch < usrp->get_rx_num_channels(); ch++) {
                uhd::tune_result_t tune_res = usrp->set_rx_freq(tune_req, ch);
                logger->log(tune_res.to_pp_string(), Log::DEBUG);
            }

            // wait for all local oscillators to lock
            for (size_t ch = 0; ch < usrp->get_rx_num_channels(); ch++) {
                while (!usrp->get_rx_sensor("lo_locked", ch).to_bool())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!cfg.tx.subdev.empty()) {
            // prepare tune request
            uhd::tune_request_t tune_req {cfg.tx.freq_rf};
            tune_req.args["mode_n"] = "integer";
            tune_req.target_freq = cfg.tx.freq_rf;
            tune_req.rf_freq = cfg.tx.freq_rf;
            tune_req.dsp_freq = cfg.tx.freq_dsp;
            tune_req.rf_freq_policy  = uhd::tune_request_t::POLICY_MANUAL;
            tune_req.dsp_freq_policy = uhd::tune_request_t::POLICY_MANUAL;

            for (size_t ch = 0; ch < usrp->get_tx_num_channels(); ch++) {
                uhd::tune_result_t tune_res = usrp->set_tx_freq(tune_req, ch);
                logger->log(tune_res.to_pp_string(), Log::DEBUG);
            }

            // wait for all local oscillators to lock
            for (size_t ch = 0; ch < usrp->get_tx_num_channels(); ch++) {
                while (!usrp->get_tx_sensor("lo_locked", ch).to_bool())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // set gains after tuning to minimize emission of local oscillator
        // leakage into bands off the the target RF frequency
        if (!cfg.rx.subdev.empty()) {
            usrp->set_rx_gain(cfg.rx.gain);
        }
        if (!cfg.tx.subdev.empty()) {
            usrp->set_tx_gain(cfg.tx.gain);
        }

        // log USRP hardware and channel configuration
        logger->log_usrp_hardware(usrp);
        logger->log_usrp_channels(usrp);

        // start Rx and Tx threads
        // NOTE: allocate 2M hugepages via `echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`
        Rx::sptr rx;
        if (!cfg.rx.subdev.empty())
            rx = std::make_shared<Rx>(usrp, logger, cfg);
        Tx::sptr tx;
        if (!cfg.tx.subdev.empty())
            tx = std::make_shared<Tx>(usrp, logger, cfg);

        // prepare file writing
        Wr::sptr wr;
        std::filesystem::path wr_dir = cfg.wr.directory;

        logger->log("Initialization succeeded.");

        // run until interrupted
        while (true) {
            // signal handling with timeout (break loop on SIGINT, or SIGTERM,
            // but ignore SIGHUP)
            const struct timespec timeout = { .tv_sec = 0, .tv_nsec = 100'000'000UL };
            int ret = sigtimedwait(&set, nullptr, &timeout);
            if (ret == -1 && errno != EAGAIN && errno != EINTR) {
                throw syscall_error{"sigtimedwait() failed"};
            } else if (ret == SIGINT || ret == SIGTERM) {
                logger->log("Received SIGINT or SIGTERM. Exiting gracefully.");
                break;
            } else if (ret == SIGUSR1) {
                logger->log("Received SIGUSR1. Toggling file writing.");
                if (wr) {
                    wr.reset();
                } else {
                    wr = std::make_shared<Wr>(
                        rx->get_ringbufs(), logger,
                        wr_dir / ("rx_" + cfg.usrp.args),
                        (usrp->get_time_now().get_full_secs() + 1) * 1'000'000'000UL
                    );
                }
            } else if (ret == SIGUSR2) {
                logger->log("Received SIGUSR2. Toggling Tx muting.");
                tx->toggle_mute();
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

            if (rx && !rx->is_running()) {
                logger->log("Rx thread stopped unexpectedly. Terminating.",
                            Log::FATAL);
                exit_code = 1;
                break;
            }

            if (tx && !tx->is_running()) {
                logger->log("Tx thread stopped unexpectedly. Terminating.",
                            Log::FATAL);
                exit_code = 1;
                break;
            }

            if (wr && !wr->is_running()) {
                logger->log("Wr thread stopped unexpectedly. File writing failed.",
                            Log::ERROR);
            }
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
