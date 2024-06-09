#ifndef RX_HPP
#define RX_HPP

#include <atomic>
#include <memory>
#include <thread>

#include <uhd/usrp/multi_usrp.hpp>

#include "config.hpp"
#include "logging.hpp"
#include "ringbuf.hpp"

class Rx {
public:
    using sptr = std::shared_ptr<Rx>;

    Rx(uhd::usrp::multi_usrp::sptr usrp,
       Logger::sptr logger,
       const Config& cfg);
    ~Rx();

    inline auto is_running(void) const -> bool
    { return run.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> run {false};
    std::thread worker_handle;

    uhd::usrp::multi_usrp::sptr usrp;
    Logger::sptr logger;

    std::vector<Ringbuf::sptr> ringbufs;

    void worker();
};

#endif /* RX_HPP */
