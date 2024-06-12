#ifndef TX_HPP
#define TX_HPP

#include <atomic>
#include <filesystem>
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

    void worker(std::vector<Tx::sample_t>&& tx_signal);
};

#endif /* TX_HPP */
