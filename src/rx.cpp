// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
    #include <sched.h>
}

#include "error.hpp"
#include "rx.hpp"

Rx::Rx(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg)
    : usrp{usrp}
    , logger{logger}
    , sample_rate_hz{usrp->get_rx_rate()}
{
    // allocate Rx ringbuffers
    for (size_t ch = 0; ch < usrp->get_rx_num_channels(); ch++) {
        std::filesystem::path desc_path {cfg.shmem.mount_desc};
        std::filesystem::path ring_path {cfg.shmem.mount_ring};
        desc_path /= "usrp_rxtx_" + cfg.usrp.args
                   + "_ch" + std::to_string(ch)
                   + "_desc";
        ring_path /= "usrp_rxtx_" + cfg.usrp.args
                   + "_ch" + std::to_string(ch)
                   + "_ring";

        Ringbuf<sample_t>::sptr ringbuf = std::make_shared<Ringbuf<sample_t>>(
            desc_path, cfg.shmem.size_desc_mib << 20,
            ring_path, cfg.shmem.size_ring_mib << 20
        );
        ringbufs.push_back(std::move(ringbuf));
    }

    // spawn worker
    worker_handle = std::thread{&Rx::worker, this};
    worker_latch.wait();
}

Rx::~Rx()
{
    // stop and join worker thread
    run.store(false, std::memory_order_relaxed);
    worker_handle.join();
}

static inline auto timespec_to_nsec(const uhd::time_spec_t& time) -> uint64_t
{
    return (uint64_t) (time.get_full_secs() * 1e9) +
           (uint64_t) (time.get_frac_secs() * 1e9);
}

void Rx::worker()
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "rx");
#endif

    // set realtime priorioty
    const struct sched_param param = {
        .sched_priority = 99
    };
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
        throw syscall_error{"sched_setscheduler() failed"};

    // setup rx streamer
    static_assert(std::is_same<sample_t, std::complex<int16_t>>::value);
    uhd::stream_args_t stream_args {"sc16", "sc16"};
    stream_args.channels = std::vector<size_t> {};
    for (size_t n = 0; n < usrp->get_rx_num_channels(); n++)
        stream_args.channels.push_back(n);
    uhd::rx_streamer::sptr rx = usrp->get_rx_stream(stream_args);

    // cache constant parameters
    const double sample_rate_hz = this->sample_rate_hz;
    const size_t num_channels = rx->get_num_channels();
    const size_t max_num_samps = rx->get_max_num_samps();

    /// buffer pointers for uhd::rx_streamer::recv()
    std::vector<void *> buf_ptrs(rx->get_num_channels());
    /// temporary buffers for samples that cannot be received directly into
    /// the Ringbufs (e.g., when relocation is needed after overflow)
    std::vector<std::vector<sample_t>> tmp_bufs(
        num_channels, std::vector<sample_t>(max_num_samps));

    /// start time on next full second at least half a second in the future
    uhd::time_spec_t start_time = usrp->get_time_now();
    if (start_time.get_frac_secs() < 0.5) {
        start_time = uhd::time_spec_t{start_time.get_full_secs() + 1, 0.};
    } else {
        start_time = uhd::time_spec_t{start_time.get_full_secs() + 2, 0.};
    }

    /// time of first sample in Ringbuf in nanoseconds
    const uint64_t start_time_nsec = timespec_to_nsec(start_time);
    for (size_t n = 0; n < num_channels; n++) {
        ringbufs[n]->set_descriptor_info(start_time_nsec, sample_rate_hz);
    }

    // Ringbufs have been fully initialized, so wake up constructor
    worker_latch.count_down();

    // start streaming
    uhd::stream_cmd_t stream_cmd {uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS};
    stream_cmd.stream_now = false;
    stream_cmd.time_spec = start_time;
    stream_cmd.num_samps = 0;
    rx->issue_stream_cmd(stream_cmd);
    logger->log_uhd_stream_cmd(stream_cmd);

    /// expected time spec of next receive in samples
    uint64_t ts_samples_next_recv = stream_cmd.time_spec.to_ticks(sample_rate_hz);

    /// timeout for uhd::rx_streamer::recv()
    double recv_timeout = 2.0;

    /// contiguous sample operation (no gap between last and next recv())
    // depending on the device, the first packet's timestamp may differ from
    // the supplied timestamp, so always start in non-contiguous mode
    uint64_t contiguous = 0;

    while (run.load(std::memory_order_relaxed)) {
        size_t recv_max_samples = max_num_samps;
        // update buf_ptrs for next uhd::rx_streamer::recv()
        if (contiguous) [[likely]] {
            // recv() contiguous data directly into Ringbufs
            for (size_t n = 0; n < num_channels; n++) {
                auto span = ringbufs[n]->get_producer_span(max_num_samps);
                buf_ptrs[n] = (void *) span.data();
                recv_max_samples = std::min(recv_max_samples, span.size());
            }
        } else {
            // recv() non-contiguous data into temporary buffer, as we need
            // the time_spec_t in rx_metadata_t to determine where to place
            // the received samples in the Rinfbufs
            for (size_t n = 0; n < num_channels; n++) {
                buf_ptrs[n] = (void *) tmp_bufs[n].data();
            }
        }

        // receive packet for each channel from USRP
        uhd::rx_metadata_t md;
        const size_t rcvd_samples = rx->recv(buf_ptrs, recv_max_samples, md, recv_timeout, true);

        // error handling
        if (rcvd_samples == 0) [[unlikely]] {
            logger->log_uhd_rx_metadata(md);

            // see: http://files.ettus.com/manual/structuhd_1_1rx__metadata__t.html
            switch (md.error_code) {
            case uhd::rx_metadata_t::ERROR_CODE_NONE:
                // should not occur. was logged above, so ignore.
                break;
            case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
                // MANUAL: An internal receive buffer has filled or a sequence
                //         error has been detected. [...]
                //         Data is missing between this time_spec and the and
                //         the time_spec of the next successful receive.
                // the next uhd::rx_streamer::recv() call will likely return
                // samples, so keep receiving. should no more samples arrive,
                // it will be caught by the timeout handling below.
                contiguous = 0;
                break;
            case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
            case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
            case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
            case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
            case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
            default:
                // presumably fatal errors. surrender and terminate the thread.
                // TODO: implement recovery by stopping and restarting the
                //       stream without causing an infinite restart deadlock.
                logger->log("Rx thread encountered irrecoverable error. Terminating.",
                            Log::FATAL);
                run.store(false, std::memory_order_relaxed);
                break;
            }

            // process next packet
            continue;
        // log first rx_metadata_t of each contiguous block of samples
        // NOTE: some UHD versions (tested with 4.3.0 and 4.6.0) do not set
        //       rx_metadata_t::start_of_burst so additionally rely on
        //       contiguous, which is always false at the beginning of a burst
        } else if (md.start_of_burst || !contiguous) [[unlikely]] {
            logger->log_uhd_rx_metadata(md);
        }

        // below: handling of samples from successful uhd::rx_streamer::recv()

        // reduce timeout of future uhd::rx_streamer::recv() calls
        recv_timeout = 0.1;

        /// time spec of this recv() in samples
        const uint64_t ts_samples_packet = md.time_spec.to_ticks(sample_rate_hz);

        // verify time specs
        // TODO: improve this; maybe log and terminate instead of assertion
        assert(md.has_time_spec);
        // FIXME: breaks reception if device starts burst prematurely
        assert(ts_samples_packet >= ts_samples_next_recv);

        if (contiguous) [[likely]] {
            // verify time specs of contiguous sample packets
            // FIXME: results in one log entry per packet if time specs do mismatch
            /*
            if (ts_samples_packet != ts_samples_next_recv) [[unlikely]] {
                // TODO: implement as Log::RxTimespecMismatch
                logger->log("Timestamp mismatch: "
                            + std::to_string(ts_samples_packet) + " != "
                            + std::to_string(ts_samples_next_recv),
                            Log::WARN);
            }
            */

            // regular receive: samples are already correctly placed in Ringbufs
            for (size_t n = 0; n < num_channels; n++) {
                ringbufs[n]->produce(rcvd_samples);
            }
        } else {
            // non-contiguous receive: use time_spec_t in rx_metadata_t to
            // determine amount of lost samples, to zero pad these, and to copy
            // the received samples to the correct location in the Ringbuf
            const uint64_t lost_samples = ts_samples_packet - ts_samples_next_recv;

            // abort if more than a second worth of samples need zero-padding
            if (lost_samples > (uint64_t) sample_rate_hz) {
                logger->log("Lost too many samples for zero-padding: "
                            + std::to_string(lost_samples)
                            + ". Terminating.", Log::FATAL);
                run.store(false, std::memory_order_relaxed);
                break;
            }

            // log zero-padding
            if (lost_samples)
                logger->log_rx_zeropad(ringbufs[0]->get_head(), lost_samples);

            for (size_t n = 0; n < num_channels; n++) {
                // zero-pad lost samples (may wrap around Ringbuf)
                size_t zeroed = 0;
                while (zeroed < lost_samples) {
                    auto span = ringbufs[n]->get_producer_span(lost_samples - zeroed);
                    assert((uint64_t) span.size() <= lost_samples);
                    std::memset((void *) span.data(), 0, span.size_bytes());
                    ringbufs[n]->produce(span.size());
                    zeroed += span.size();
                }

                // copy from temporary receive buffer into Ringbuf (may wrap)
                size_t copied = 0;
                while (copied < rcvd_samples) {
                    auto span = ringbufs[n]->get_producer_span(rcvd_samples - copied);
                    assert(span.size() <= rcvd_samples);
                    std::memcpy((void *) span.data(), (void *) &tmp_bufs[0][copied], span.size_bytes());
                    ringbufs[n]->produce(span.size());
                    copied += span.size();
                }
            }

            // account for zero-padding in expected time_spec_t of next recv()
            ts_samples_next_recv = md.time_spec.to_ticks(sample_rate_hz);
        }

        // time_spec of next recv() must advance by amount of received samples
        ts_samples_next_recv += rcvd_samples;

        // next successful uhd::rx_streamer::recv() either yields contiguous
        // samples or returns another error
        contiguous += rcvd_samples;
        continuous_samples.store(contiguous, std::memory_order_relaxed);
    }

    // stop streaming
    stream_cmd = uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
    rx->issue_stream_cmd(stream_cmd);
    logger->log_uhd_stream_cmd(stream_cmd);

    // use temporary buffers to discard received data
    size_t recv_max_samples = tmp_bufs[0].size();
    for (size_t n = 0; n < num_channels; n++)
        buf_ptrs[n] = (void *) tmp_bufs[n].data();
    // receive until md.end_of_burst or any error (e.g., timeout)
    while (true) {
        uhd::rx_metadata_t md;
        size_t samples = rx->recv(buf_ptrs, recv_max_samples, md, 0.1, true);
        if (md.end_of_burst || samples == 0) {
            logger->log_uhd_rx_metadata(md);
            break;
        }
    }

    run.store(false, std::memory_order_relaxed);
} catch (const std::exception& e) {
    logger->log_exception(std::current_exception());
    logger->log("Unrecoverable exception occurred in Rx thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
} catch (...) {
    logger->log("Unknown exception occurred in Rx thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
}
