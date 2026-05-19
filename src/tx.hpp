// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#ifndef TX_HPP
#define TX_HPP

#include <atomic>
#include <thread>

#include <uhd/usrp/multi_usrp.hpp>

#include "config.hpp"
#include "logging.hpp"

class Tx {
public:
    using sptr = std::shared_ptr<Tx>;
    using sample_t = std::complex<std::int16_t>;

    Tx(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg);
    ~Tx();

    // delete copy constructor and copy assignment operator
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;

    auto get_tx_seconds() const -> double
    { return burst_samples.load(std::memory_order_relaxed) / sample_rate_hz; }

    void toggle_mute()
    { mute.store(!is_muted(), std::memory_order_relaxed); }

    auto is_muted() const -> bool
    { return mute.load(std::memory_order_relaxed); }
    auto is_running() const -> bool
    { return run.load(std::memory_order_relaxed); }

private:
    uhd::usrp::multi_usrp::sptr usrp;
    Logger::sptr logger;

    std::atomic<bool> mute {false};
    std::atomic<bool> run {true};
    std::thread worker_handle;

    std::atomic<uint64_t> burst_samples {0};
    double sample_rate_hz {0};

    void worker(std::vector<Tx::sample_t>&& tx_signal);
};

#endif /* TX_HPP */
