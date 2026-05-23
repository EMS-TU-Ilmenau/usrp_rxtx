// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
#if _POSIX_C_SOURCE >= 200112L
    #include <fcntl.h>
#endif
}

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
            paths.push_back(path);

            // open file
            std::FILE *fh = std::fopen(path.c_str(), "wbx");
            if (fh == nullptr)
                throw syscall_error{"std::fopen(\"" + path.generic_string() + "\", ...) failed"};
            files.push_back(fh);

            // disable buffering
            std::setvbuf(fh, nullptr, _IONBF, 0);

            // set O_DIRECT on platforms that support it
#if _POSIX_C_SOURCE >= 200112L && O_DIRECT
            int fd = fileno(fh);
            if (fd < 0)
                throw syscall_error{"fileno() failed"};
            int flags = fcntl(fd, F_GETFL);
            if (flags < 0)
                throw syscall_error{"fcntl(fd, F_GETFL) failed"};
            if (fcntl(fd, F_SETFL, flags | O_DIRECT) != 0)
                throw syscall_error{"fcntl(fd, F_SETFL) failed"};
#endif

            // log file path, start time, and Ringbuf offset
            logger->log_wr_open(path, start_nsec, tails[ch]);
        }
    } catch (...) {
        // close all open file handles and rethrow exception
        for (auto fh : files)
            fclose(fh);
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

    // close all open file handles
    for (auto fh : files)
        fclose(fh);
}

void Wr::worker()
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "wr");
#endif

    assert(files.size() == ringbufs.size());

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
            // fwrite() should return 0 only if size == 0 or count == 0
            assert(span.size_bytes() > 0);
            size_t ret = std::fwrite((char *) span.data(), 1, span.size_bytes(), files[ch]);
            if (ret == 0)
                throw syscall_error{"std::fwrite() failed"};

            // check if span was clobbered during write()
            if (ringbufs[ch]->get_clobber_distance(tails[ch]) < 0) {
                // truncate clobbered block
                std::filesystem::resize_file(paths[ch], sizeof(Wr::sample_t) * _samples_written);
                throw generic_error{"Filesystem writing too slow"};
            }

            // update tail pointer and sleep duration
            tails[ch] += ret / sizeof(Wr::sample_t);

            // increment number of written samples
            if (ch == ringbufs.size() - 1)
                _samples_written += ret / sizeof(Wr::sample_t);
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
