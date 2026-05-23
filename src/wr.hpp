// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#ifndef WR_HPP
#define WR_HPP

#include <atomic>
#include <cstdio>
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

    auto get_backlog_bytes() const -> uint64_t
    { return backlog_samples.load(std::memory_order_relaxed) * sizeof(sample_t); }

    auto get_wr_seconds() const -> double
    { return samples_written.load(std::memory_order_relaxed) / sample_rate_hz; }

    auto is_running() const -> bool
    { return run.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> run {true};
    std::thread worker_handle;

    std::atomic<uint64_t> backlog_samples {0};
    std::atomic<uint64_t> samples_written {0};
    double sample_rate_hz {0.};

    std::vector<Ringbuf<sample_t>::sptr> ringbufs;
    Logger::sptr logger;

    std::vector<std::filesystem::path> paths;
    std::vector<std::FILE *> files;
    std::vector<uint64_t> tails;

    void worker();
};

#endif /* WR_HPP */
