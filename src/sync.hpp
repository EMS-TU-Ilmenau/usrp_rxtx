#ifndef SYNC_HPP
#define SYNC_HPP

#include <atomic>
#include <memory>
#include <thread>

#include "config.hpp"
#include "logging.hpp"

class Sync {
public:
    using sptr = std::shared_ptr<Sync>;

    Sync(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg);
    ~Sync();

    void sync_10mhz();
    void sync_1pps();
    auto wait_pps() -> uhd::time_spec_t;

private:
    std::atomic<bool> run {true};
    std::thread worker_handle;

    uhd::usrp::multi_usrp::sptr usrp;
    Logger::sptr logger;

    void worker();
};

#endif /* SYNC_HPP */
