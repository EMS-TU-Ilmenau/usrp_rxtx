// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#include <cstring>
#include <type_traits>
#include <uhd/types/metadata.hpp>

extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
#if _POSIX_C_SOURCE >= 200112L
    #include <sched.h>
#endif
}

#include <uhd/usrp/multi_usrp.hpp>

#include "error.hpp"
#include "tx.hpp"

/// minimal number of frames to coalesce per tx_stream_t::send() call
// TODO: make this tunable run-time configurable
static constexpr size_t FRAMES_PER_SEND = 128;

/// delay in seconds of start-of-burst timestamp after it's sent
// TODO: make this tunable run-time configurable
static constexpr double BURST_DELAY = 0.01;

Tx::Tx(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg)
    : usrp{usrp}
    , logger{logger}
    , sample_rate_hz{usrp->get_tx_rate()}
{
    // read sample file into buffer
    // TODO: separate file for each Tx channel
    size_t tx_samples = std::filesystem::file_size(cfg.tx.file) / sizeof(sample_t);
    std::vector<sample_t> tx_signal(tx_samples);
    std::ifstream file {cfg.tx.file, std::ios_base::in | std::ios_base::binary};
    if (!file)
        throw generic_error{"failed to open Tx file: " + cfg.tx.file};
    file.read((char *) tx_signal.data(), tx_signal.size() * sizeof(sample_t));
    if (!file)
        throw generic_error{"failed to read Tx file: " + cfg.tx.file};
    file.close();

    // log number of samples read from file
    std::stringstream msg;
    msg << "Tx signal loaded from file " << std::quoted(cfg.tx.file)
        << " (" << tx_signal.size() << " samples).";
    logger->log(msg.str(), Log::DEBUG);

    // spawn worker
    worker_handle = std::thread(&Tx::worker, this, tx_signal);
}

Tx::~Tx()
{
    // stop and join worker thread
    run.store(false, std::memory_order_relaxed);
    worker_handle.join();
}

/// round up time_spec_t to signal period
static inline uhd::time_spec_t
align_time(const uhd::time_spec_t& ts, double delay_sec,
           double sample_rate_hz, size_t period_samples);

void Tx::worker(std::vector<sample_t>&& tx_signal)
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "tx");
#endif

#if _POSIX_C_SOURCE >= 200112L
    // set realtime priority
    const struct sched_param param = {
        .sched_priority = 99
    };
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
        throw syscall_error{"sched_setscheduler() failed"};
#endif

    // setup Tx streamer
    static_assert(std::is_same<sample_t, std::complex<int16_t>>::value);
    uhd::stream_args_t stream_args {"sc16", "sc16"};
    stream_args.args["underflow_policy"] = "next_burst";
    stream_args.channels = std::vector<size_t> {};
    for (size_t n = 0; n < usrp->get_tx_num_channels(); n++)
        stream_args.channels.push_back(n);
    uhd::tx_streamer::sptr tx = usrp->get_tx_stream(stream_args);

    // retrieve streaming parameters
    const uint64_t rate_hz = this->sample_rate_hz;
    const size_t num_channels = tx->get_num_channels();
    const uint64_t samps_per_frame = tx->get_max_num_samps();

    // bug flags used to trigger special treatment of misbehaving devices (see
    // occurrences of variable names below for details)
    const bool bug_error_burst_ack = usrp->get_mboard_name()[0] == 'B';

    // tx_signal contains a single period of the transmit signal. create a
    // periodified copy for use with uhd::tx_streamer::send().
    std::vector<sample_t> sendbuf(FRAMES_PER_SEND * samps_per_frame + tx_signal.size());
    for (size_t n = 0; n < sendbuf.size(); n++) {
        sendbuf[n] = tx_signal[n % tx_signal.size()];
    }

    /// buffer pointers for uhd::tx_streamer::send()
    std::vector<const void *> buf_ptrs {num_channels};

    /// successfully transmitted samples in this burst (0 := start of burst)
    uint64_t samples_burst = 0;
    uhd::tx_metadata_t tx_md;
    while (run.load(std::memory_order_relaxed)) {
        if (samples_burst == 0) [[unlikely]] {
            tx_md.has_time_spec  = true;
            tx_md.start_of_burst = true;
            tx_md.end_of_burst   = false;
            tx_md.time_spec      = align_time(usrp->get_time_now(), BURST_DELAY,
                                              rate_hz, tx_signal.size());

            logger->log_uhd_tx_metadata(tx_md);
        }

        // update pointers into sendbuf to ensure signal periodicity
        // TODO: separate sequence for each Tx channel
        for (size_t n = 0; n < num_channels; n++)
            buf_ptrs[n] = &sendbuf[samples_burst % tx_signal.size()];

        // stream buffer to USRP
        size_t num_tx_samps = tx->send(buf_ptrs, FRAMES_PER_SEND * samps_per_frame, tx_md, 1.);
        samples_burst += num_tx_samps;
        if (num_tx_samps == 0) [[unlikely]] {
            logger->log("Tx uhd::tx_streamer::send() timed out.", Log::ERROR);
        }

        // reset metadata for subsequent iterations (may be changed by error
        // handling below)
        tx_md.has_time_spec  = false;
        tx_md.start_of_burst = false;
        tx_md.end_of_burst   = false;
        tx_md.time_spec      = uhd::time_spec_t{};

        // check for async errors and restart burst if at least one occurred
        uhd::async_metadata_t async_md;
        while (tx->recv_async_msg(async_md, 0.)) [[unlikely]] {
            // log all received async_metadata_t
            logger->log_uhd_async_metadata(async_md);

            switch (async_md.event_code) {
            // ignore burst acks and user payload
            // FIXME: BURST_ACK isn't an error, but it should not occur now.
            //        regardless, it is simply ignored here ...
            case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
            case uhd::async_metadata_t::EVENT_CODE_USER_PAYLOAD:
                continue;
            // set error condition for all remaining event codes
            case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
            case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR:
            case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
            case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
            case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST:
            default:
                samples_burst = 0;
            }
        }

        burst_samples.store(samples_burst, std::memory_order_relaxed);

        // If a streaming error occurred (e.g., buffer underflow or packet loss),
        // the USRP does not ensure coherent sampling by zero padding lost
        // samples and discarding late samples. The only way to mitigate that
        // and to ensure coherent Tx sample streaming is to stop the burst after
        // an error occurred and to start a new burst with an appropriately set
        // time_spec.

        // stop burst on error or when muting/termination was requested
        if (!samples_burst ||
            mute.load(std::memory_order_relaxed) ||
            !run.load(std::memory_order_relaxed)
        ) [[unlikely]] {
            // send a mini end-of-burst (EOB) packet without any sample payload
            tx_md.has_time_spec  = false;
            tx_md.start_of_burst = false;
            tx_md.end_of_burst   = true;
            tx->send(buf_ptrs, 0, tx_md);

            // log EOB
            logger->log_uhd_tx_metadata(tx_md);

            // After sending the EOB, the USRP's fifo will likely still contain
            // samples that it must process before the burst is complete.
            // Starting a new burst while the previous burst is still in
            // progress causes undefined USRP behavior (tests revealed that the
            // time_spec of the subsequent burst will be ignored).
            // When the burst has been completed, the USRP sends an async
            // metadata packet with EVENT_CODE_BURST_ACK.

            // receive async BURST_ACKs for all channels
            size_t num_acks = 0;

            // Note that while the USRP X310 behaves as described above, the
            // USRP B205 does **NOT** send an EVENT_CODE_BURST_ACK for an
            // end_of_burst following an error, e.g., EVENT_CODE_UNDERFLOW.
            // All end_of_burst packets not preceded by an error code are
            // acknowledged as expected.
            // Therefore, avoid unnecessary timeouts by only waiting for
            // EVENT_CODE_BURST_ACK if this is either not a a buggy device or
            // not an error.
            if (!samples_burst && bug_error_burst_ack)
                num_acks = num_channels;
            while (num_acks < num_channels) {
                // FIXME: hard-coded timeout
                if (tx->recv_async_msg(async_md, 0.1)) {
                    logger->log_uhd_async_metadata(async_md);
                    if (async_md.event_code == uhd::async_metadata_t::EVENT_CODE_BURST_ACK)
                        num_acks++;
                } else {
                    break;
                }
            }

            // reset samples_burst so next main loop iteration starts with a
            // new burst
            samples_burst = 0;
            burst_samples.store(0, std::memory_order_relaxed);

            // process mute request
            if (mute.load(std::memory_order_relaxed)) {
                // zero gain to minimize LO leakage while muted
                double gain = usrp->get_tx_gain();
                usrp->set_tx_gain(0.);

                // poll for mute to be lifted while also processing async_metadata_t
                while (run.load(std::memory_order_relaxed) && mute.load(std::memory_order_relaxed)) {
                    while (tx->recv_async_msg(async_md, 0.001))
                        logger->log_uhd_async_metadata(async_md);
                }

                // restore gain
                usrp->set_tx_gain(gain);
            }
        }
    }

    run.store(false, std::memory_order_relaxed);
} catch (const std::exception& e) {
    logger->log_exception(std::current_exception());
    logger->log("Unrecoverable exception occurred in Tx thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
} catch (...) {
    logger->log("Unknown exception occurred in Tx thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
}

/// round up time_spec_t to signal period
static inline uhd::time_spec_t
align_time(const uhd::time_spec_t& ts, double delay_sec,
           double sample_rate_hz, size_t period_samples)
{
    uint64_t sample_rate_hz_int  = sample_rate_hz;
    double   sample_rate_hz_frac = sample_rate_hz - sample_rate_hz_int;

    if (sample_rate_hz_frac == 0.) {
        // convert time_spec_t + delay into number of samples since unix epoch
        uint64_t samples = (uint64_t) (ts.get_full_secs() * sample_rate_hz_int)
                         + (uint64_t) (ts.get_frac_secs() * sample_rate_hz_int)
                         + (uint64_t) (sample_rate_hz * delay_sec);

        // align sample count to next multiple of signal period
        samples = (samples + period_samples - 1)
                / period_samples * period_samples;

        // convert sample count back into uhd::time_spec_t
        return uhd::time_spec_t(samples / sample_rate_hz_int,
                                (samples % sample_rate_hz_int) / sample_rate_hz);
    } else {
        // TODO: implement non-integer sample rates
        throw generic_error{"Non-integer sample rates are not implemented"};
    }
}
