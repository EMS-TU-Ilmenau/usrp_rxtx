// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
    #include <fcntl.h>
    #include <sys/resource.h>
    #include <sys/stat.h>
    #include <sys/types.h>
}

#include "config.hpp"
#include "error.hpp"
#include "wr.hpp"

static constexpr size_t WR_BLOCK_SAMPLES = 4 * 1024 * 1024 / sizeof(Wr::sample_t);

Wr::Wr(std::vector<Ringbuf<sample_t>::sptr>&& _ringbufs,
       Logger::sptr logger,
       const std::filesystem::path& path_prefix,
       const uint64_t start_nsec)
    : ringbufs{std::move(_ringbufs)}
    , logger{logger}
{
    // verify that all Ringbufs are synchronous
    for (size_t n = 1; n < ringbufs.size(); n++) {
        if (ringbufs[n]->get_start_nsec() != ringbufs[0]->get_start_nsec())
            throw generic_error{"ringbuf start times differ"};
        if (ringbufs[n]->get_sample_rate_hz() != ringbufs[0]->get_sample_rate_hz())
            throw generic_error{"ringbuf sample rates differ"};
    }

    sample_rate_hz = ringbufs[0]->get_sample_rate_hz();

    try {
        for (size_t ch = 0; ch < ringbufs.size(); ch++) {
            // get Ringbuf tail for timestamp (may throw)
            tails.push_back(ringbufs[ch]->get_tail(start_nsec));

            // suffix filename with start time and channel index
            std::stringstream filename;
            filename << "_" << start_nsec << "_ch" << ch << ".cint16.bin";
            std::filesystem::path path = path_prefix.generic_string() + filename.str();

            // open file
            int fd;
            if ((fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_DIRECT, 0644)) == -1)
                throw syscall_error{"open(\"" + path.generic_string() + "\", ...) failed"};
            fds.push_back(fd);

            // log file path, start time, and Ringbuf offset
            logger->log_wr_open(path, start_nsec, tails[ch]);
        }
    } catch (...) {
        // close all opened fds and rethrow exception
        for (size_t ch = 0; ch < fds.size(); ch++)
            close(fds[ch]);
        throw;
    }

    // spawn worker
    worker_handle = std::thread{&Wr::worker, this};
}

Wr::~Wr()
{
    // stop and join worker thread
    run.store(false, std::memory_order_relaxed);
    worker_handle.join();

    // close all open file descriptors
    for (size_t ch = 0; ch < fds.size(); ch++)
        close(fds[ch]);
}

void Wr::worker()
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "wr");
#endif

    assert(fds.size() == ringbufs.size());

    uint64_t _samples_written = 0;
    while (run.load(std::memory_order_relaxed)) {
        /// sleep in nanoseconds until next iteration
        uint64_t sleep_ns = 10'000'000UL;

        uint64_t _backlog_samples = 0;

        // sequentially write WR_BLOCK_SAMPLES from each Ringbuf
        for (size_t ch = 0; ch < ringbufs.size(); ch++) {
            auto span = ringbufs[ch]->get_consumer_span(tails[ch], WR_BLOCK_SAMPLES);

            // only update sleep duration if this Ringbuf yields empty span
            if (span.size() == 0) {
                sleep_ns = std::min(sleep_ns, ringbufs[ch]->get_consumer_wait(tails[ch], WR_BLOCK_SAMPLES));
                continue;
            } else {
                sleep_ns = 0;
            }

            // write span to respective fd
            // write(fd, buf, size) on a file should return 0 only if size == 0
            ssize_t ret = write(fds[ch], (char *) span.data(), span.size_bytes());
            if (ret < 0)
                throw syscall_error{"write() failed"};
            if (ret == 0)
                throw generic_error{"write() returned 0"};

            // check if span was clobbered during write()
            // TODO: ftruncate() the potentially clobbered bytes away
            if (ringbufs[ch]->get_clobber_distance(tails[ch]) < 0)
                throw generic_error{"write() too slow: end of file was clobbered"};

            // update tail pointer and sleep duration
            tails[ch] += ret / sizeof(sample_t);

            // increment number of written samples
            if (ch == 0)
                _samples_written += ret / sizeof(sample_t);
        }

        for (size_t ch = 0; ch < ringbufs.size(); ch++)
            _backlog_samples += ringbufs[ch]->get_backlog_samples(tails[ch]);
        backlog_samples.store(_backlog_samples, std::memory_order_relaxed);
        samples_written.store(_samples_written, std::memory_order_relaxed);

        // sleep until at least one Ringbuf should return a consumer span
        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }

    run.store(false, std::memory_order_relaxed);
} catch (const std::exception& e) {
    logger->log_exception(std::current_exception());
    logger->log("Unrecoverable exception occurred in Wr thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
} catch (...) {
    logger->log("Unknown exception occurred in Wr thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
}
