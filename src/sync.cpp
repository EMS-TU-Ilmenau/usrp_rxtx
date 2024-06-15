#include <csignal>
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

    logger->log("Set USRP time to: " + std::to_string(pps.get_real_secs()));
}

auto Sync::wait_pps() -> uhd::time_spec_t
{
    uhd::time_spec_t now  = usrp->get_time_last_pps();
    uhd::time_spec_t last = now;

    // FIXME: not interruptible
    while (now == last) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        last = now;
        now  = usrp->get_time_last_pps();
    }

    return now;
}
