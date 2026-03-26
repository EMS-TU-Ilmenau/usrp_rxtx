// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#include <algorithm>
#include <cstring>
#include <type_traits>

extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
    #include <sched.h>
}

#include <uhd/usrp/multi_usrp.hpp>

#include "error.hpp"
#include "tx.hpp"

/// minimal number of frames to coalesce per tx_stream_t::send() call
#define FRAMES_PER_SEND 128

/// seconds to wait for EVENT_CODE_BURST_ACK after sending end-of-burst
// FIXME: May be too short for very low sample rates and is way too long for
//        high sample rates. Timeout should be sample rate dependent.
#define TIMEOUT_BURST_ACK 2.

/// delay in seconds of start-of-burst timestamp after it's sent
#define BURST_HOLDOFF 0.01

Tx::Tx(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg)
    : usrp{usrp}
    , logger{logger}
    , sample_rate_hz{usrp->get_tx_rate()}
{
    // TODO: implement support for multiple Tx channels
    if (usrp->get_tx_num_channels() != 1)
        throw std::invalid_argument{"Tx supports single channel operation only"};

    // read sample file into buffer
    size_t tx_samples = std::filesystem::file_size(cfg.tx.file) / sizeof(sample_t);
    std::vector<sample_t> tx_signal(tx_samples);
    std::ifstream file {cfg.tx.file, std::ios_base::in | std::ios_base::binary};
    if (!file)
        throw std::runtime_error{"failed to open Tx file: " + cfg.tx.file};
    file.read((char *) tx_signal.data(), tx_signal.size() * sizeof(sample_t));
    if (!file)
        throw std::runtime_error{"failed to read Tx file: " + cfg.tx.file};
    file.close();

    // log number of samples read from file
    std::stringstream msg;
    msg << "Read " << tx_signal.size() << " samples from file " << cfg.tx.file << '.';
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
align_time(const uhd::time_spec_t& ts, double holdoff_sec,
           double sample_rate_hz, size_t period_samples);

void Tx::worker(std::vector<sample_t>&& tx_signal)
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "tx");
#endif

    // set realtime priorioty
    const struct sched_param param = {
        .sched_priority = 99
    };
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1)
        throw syscall_error{"sched_setscheduler() failed"};

    // setup Tx streamer
    static_assert(std::is_same<sample_t, std::complex<int16_t>>::value);
    uhd::stream_args_t stream_args {"sc16", "sc16"};
    stream_args.args["underflow_policy"] = "next_burst";
    stream_args.channels = std::vector<size_t> {0};
    uhd::tx_streamer::sptr tx = usrp->get_tx_stream(stream_args);

    // retrieve streaming parameters
    const uint64_t rate_hz = this->sample_rate_hz;
    const uint64_t samps_per_frame = tx->get_max_num_samps();

    // tx_signal contains a single period of the transmit signal. create a
    // periodified copy for use with uhd::tx_streamer::send().
    std::vector<sample_t> sendbuf(FRAMES_PER_SEND * samps_per_frame + tx_signal.size());
    for (size_t n = 0; n < sendbuf.size(); n++) {
        sendbuf[n] = tx_signal[n % tx_signal.size()];
    }

    /// buffer pointers for uhd::tx_streamer::send()
    std::vector<const void *> buf_ptrs {tx->get_num_channels()};

    /// successfully transmitted samples in this burst (0 := start of burst)
    uint64_t samples_burst = 0;
    uhd::tx_metadata_t tx_md;
    while (run.load(std::memory_order_relaxed)) {
        //
        if (BOOST_UNLIKELY(samples_burst == 0)) {
            tx_md.has_time_spec  = true;
            tx_md.start_of_burst = true;
            tx_md.end_of_burst   = false;
            tx_md.time_spec      = align_time(usrp->get_time_now(), BURST_HOLDOFF, rate_hz,
                                        tx_signal.size());

            logger->log_uhd_tx_metadata(tx_md);
        }

        // update pointers into sendbuf to ensure signal periodicity
        // TODO: implement multi-channel support
        buf_ptrs[0] = &sendbuf[samples_burst % tx_signal.size()];

        // stream buffer to USRP
        size_t num_tx_samps = tx->send(buf_ptrs, FRAMES_PER_SEND * samps_per_frame, tx_md, 1.);
        samples_burst += num_tx_samps;

        // reset metadata for subsequent iterations (may be changed by error
        // handling below)
        tx_md.has_time_spec  = false;
        tx_md.start_of_burst = false;
        tx_md.end_of_burst   = false;
        tx_md.time_spec      = uhd::time_spec_t{};

        // check for async errors and restart burst if at least one occurred
        uhd::async_metadata_t async_md;
        while (tx->recv_async_msg(async_md, 0.)) {
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
        // the USRP does not ensure coherent sampling by zeropadding lost
        // samples and discarding late samples. The only way to mitigate that
        // and to ensure coherent Tx sample streaming is to stop the burst after
        // an error occured and to start a new burst with an appropriately set
        // time_spec.

        // stop burst on error or when muting/termination was requested
        if (!samples_burst || mute || !run) {
            // send a mini end-of-burst (EOB) packet without any sample payload
            tx_md.has_time_spec  = false;
            tx_md.start_of_burst = false;
            tx_md.end_of_burst   = true;
            tx->send("", 0, tx_md);

            // log EOB
            logger->log_uhd_tx_metadata(tx_md);

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
                    // yields the EVENT_CODE_BURST_ACK for above EOB (one per channel)
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
align_time(const uhd::time_spec_t& ts, double holdoff_sec,
           double sample_rate_hz, size_t period_samples)
{
    uint64_t sample_rate_hz_int  = sample_rate_hz;
    double   sample_rate_hz_frac = sample_rate_hz - sample_rate_hz_int;

    if (sample_rate_hz_frac == 0.) {
        // convert time_spec_t + holdoff into number of samples since unix epoch
        uint64_t samples = ts.get_full_secs() * sample_rate_hz_int
                         + (uint64_t) (ts.get_frac_secs() * sample_rate_hz_int)
                         + (uint64_t) (sample_rate_hz * holdoff_sec);

        // align sample count to next multiple of signal period
        samples = (samples + period_samples - 1)
                / period_samples * period_samples;

        // convert sample count back into uhd::time_spec_t
        return uhd::time_spec_t(samples / sample_rate_hz_int,
                                (samples % sample_rate_hz_int) / sample_rate_hz);
    } else {
        // TODO: implement non-integer sample rates
        throw std::invalid_argument{"Non-integer sample rates are not implemented"};
    }
}
