#include <csignal>
#include <thread>

#include "error.hpp"
#include "sync.hpp"
#include "tty.hpp"

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

void Sync::sync_gpsdo()
{
    // select internal GPSDO as clock and time source
    usrp->set_clock_source("gpsdo");
    usrp->set_time_source("gpsdo");

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

    logger->log("Set USRP time to: " + std::to_string(pps.get_real_secs()));
}

void Sync::sync_b205(const std::filesystem::path& tty_path)
{
    TTY tty {tty_path, B115200};
    tty.wr("CLK:MHz");
    tty.wr("CLK:PPS");

    // switch REF to 1PPS
    tty.wr("CLK:PPS");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // set time at next external PPS pulse and wait for it
    sync_1pps();

    // switch REF to 10MHz for clock
    usrp->set_time_source("internal");
    tty.wr("CLK:MHz");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // use external clock
    sync_10mhz();
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
