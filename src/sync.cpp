// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#include <chrono>
#include <thread>

#include "error.hpp"
#include "sync.hpp"

Sync::Sync(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg)
    : usrp{usrp}
    , logger{logger}
{
    (void) cfg;
};

Sync::~Sync()
{};

/// convert uhd::time_spec_t to std::chrono::time_point
static inline auto
uhd_timespec_to_std_time_point(const uhd::time_spec_t& time)
    -> const std::chrono::time_point<std::chrono::system_clock>
{
    using namespace std::chrono;
    using unix_ns = std::chrono::time_point<system_clock, nanoseconds>;

    uint64_t nsec = (uint64_t) (time.get_full_secs() * 1e9) +
                    (uint64_t) (time.get_frac_secs() * 1e9);
    return unix_ns{nanoseconds{nsec}};
}

void Sync::sync_10mhz()
{
    // select external 10 MHz source
    usrp->set_clock_source("external");

    // wait for PLL lock
    // FIXME: not interruptible
    while (!usrp->get_mboard_sensor("ref_locked").to_bool()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Sync::sync_1pps()
{
    // select external 1 PPS source
    usrp->set_time_source("external");

    // reset time 2 pulses in the future (to facilitate synchronous reset)
    wait_pps();

    // set usrp time to system clock rounded to closest full second
    std::chrono::time_point<std::chrono::system_clock> now {std::chrono::system_clock::now()};
    auto seconds = std::chrono::round<std::chrono::seconds>(now);
    usrp->set_time_next_pps(uhd::time_spec_t{
        seconds.time_since_epoch().count() + 1, 0.
    });

    // wait for clock to be set
    uhd::time_spec_t pps = wait_pps();

    // log synchronization result
    logger->log(std::format("Synchronized USRP to host time and external PPS: {:%Y-%m-%dT%H:%M:%S}Z",
        uhd_timespec_to_std_time_point(pps)));
}

void Sync::sync_host()
{
    // set usrp time to system clock
    std::chrono::time_point<std::chrono::system_clock> now {std::chrono::system_clock::now()};
    auto duration = now.time_since_epoch();

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    duration -= seconds;
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    // TODO: pre-adjust for half of round trip time
    usrp->set_time_now(uhd::time_spec_t{
        seconds.count(), nanoseconds.count() * 1e-9
    });

    // log synchronization result
    logger->log(std::format("Synchronized USRP to host time: {:%Y-%m-%dT%H:%M:%S}Z", now));
}

void Sync::sync_gpsdo()
{
    // select internal GPSDO as clock and time source
    usrp->set_clock_source("gpsdo");
    usrp->set_time_source("gpsdo");

    // wait for PLL lock
    // FIXME: not interruptible
    while (!usrp->get_mboard_sensor("ref_locked").to_bool()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!usrp->get_mboard_sensor("gps_locked").to_bool())
        logger->log("Synchronizing to unlocked GPSDO.", Log::ERROR);

    // reset time 2 pulses in the future (to facilitate synchronous reset)
    wait_pps();

    // set usrp time to system clock rounded to closest full second
    std::chrono::time_point<std::chrono::system_clock> now {std::chrono::system_clock::now()};
    auto seconds = std::chrono::round<std::chrono::seconds>(now);
    usrp->set_time_next_pps(uhd::time_spec_t{
        seconds.time_since_epoch().count() + 1, 0.
    });

    // wait for clock to be set
    uhd::time_spec_t pps = wait_pps();

    // log synchronization result
    logger->log(std::format("Synchronized USRP to host time and internal GPSDO PPS: {:%Y-%m-%dT%H:%M:%S}Z",
        uhd_timespec_to_std_time_point(pps)));
}

auto Sync::wait_pps() -> uhd::time_spec_t
{
    uhd::time_spec_t now  = usrp->get_time_last_pps();
    uhd::time_spec_t last = now;

    // use steady_clock to measure timeout, because USRP time may jump,
    // e.g., when setting USRP time at next PPS edge
    const auto timeout = std::chrono::steady_clock::now()
                       + std::chrono::seconds(2);

    // wait for next PPS
    while (now == last) {
        if (std::chrono::steady_clock::now() >= timeout)
            throw generic_error{"Timed out waiting for PPS signal."};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        now = usrp->get_time_last_pps();
    }

    return now;
}
