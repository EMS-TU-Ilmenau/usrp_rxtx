#ifndef WR_HPP
#define WR_HPP

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

#include "logging.hpp"
#include "ringbuf.hpp"

class Wr {
public:
    using sptr = std::shared_ptr<Wr>;
    using sample_t = std::complex<int16_t>;

    Wr(std::vector<Ringbuf<sample_t>::sptr>&& ringbufs,
       Logger::sptr logger,
       const std::filesystem::path& path_prefix,
       const uint64_t start_nsec);
    ~Wr();

    auto get_backlog_samples() const -> uint64_t
    { return backlog_samples.load(std::memory_order_relaxed); }

    auto is_running() const -> bool
    { return run.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> run {true};
    std::thread worker_handle;

    std::atomic<uint64_t> backlog_samples;

    std::vector<Ringbuf<sample_t>::sptr> ringbufs;
    Logger::sptr logger;

    std::vector<int> fds;
    std::vector<uint64_t> tails;

    void worker();
};

#endif /* WR_HPP */
