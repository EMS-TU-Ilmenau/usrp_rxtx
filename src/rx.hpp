// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#ifndef RX_HPP
#define RX_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <latch>

#include <uhd/usrp/multi_usrp.hpp>

#include "config.hpp"
#include "logging.hpp"
#include "ringbuf.hpp"

class Rx {
public:
    using sptr = std::shared_ptr<Rx>;
    using sample_t = std::complex<std::int16_t>;

    Rx(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg);
    ~Rx();

    // delete copy constructor and copy assignment operator
    Rx(const Rx&) = delete;
    Rx& operator=(const Rx&) = delete;

    auto get_rx_seconds() const -> double
    { return continuous_samples.load(std::memory_order_relaxed) / sample_rate_hz; }

    auto get_ringbufs() const -> std::vector<Ringbuf<sample_t>::sptr>
    { return ringbufs; }

    auto is_running() const -> bool
    { return run.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> run {true};
    std::thread worker_handle;
    std::latch worker_latch {1};

    uhd::usrp::multi_usrp::sptr usrp;
    Logger::sptr logger;

    std::atomic<uint64_t> continuous_samples {0};
    double sample_rate_hz {0};

    std::vector<Ringbuf<sample_t>::sptr> ringbufs;

    void worker();
};

#endif /* RX_HPP */
