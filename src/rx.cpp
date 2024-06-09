#include "error.hpp"
#include "rx.hpp"

Rx::Rx(uhd::usrp::multi_usrp::sptr usrp, Logger::sptr logger, const Config& cfg)
    : usrp{usrp}
    , logger{logger}
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

        Ringbuf::sptr ringbuf = std::make_shared<Ringbuf>(
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
    constexpr size_t sample_size = sizeof(std::complex<int16_t>);
    uhd::stream_args_t stream_args {"sc16", "sc16"};
    stream_args.channels = std::vector<size_t> {};
    for (size_t n = 0; n < usrp->get_rx_num_channels(); n++)
        stream_args.channels.push_back(n);
    uhd::rx_streamer::sptr rx = usrp->get_rx_stream(stream_args);

    // cache constant parameters
    const double sample_rate_hz = usrp->get_rx_rate();
    const size_t num_channels = rx->get_num_channels();
    const size_t max_num_samps = rx->get_max_num_samps();

    /// buffer pointers for uhd::rx_streamer::recv()
    std::vector<void *> buf_ptrs(rx->get_num_channels());
    /// temporary buffers for samples that cannot be received directly into
    /// the Ringbufs (e.g., when relocation is needed after overflow)
    std::vector<std::vector<std::complex<int16_t>>> tmp_bufs(
        num_channels, std::vector<std::complex<int16_t>>(max_num_samps));

    /// start time on next full second at least half a second in the future
    uhd::time_spec_t start_time = usrp->get_time_now();
    if (start_time.get_frac_secs() < 0.5) {
        start_time = uhd::time_spec_t(start_time.get_full_secs() + 1, 0.);
    } else {
        start_time = uhd::time_spec_t(start_time.get_full_secs() + 2, 0.);
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

    /// timeout for uhd::rx_streamer::recv()
    double recv_timeout = 2.0;

    while (run.load(std::memory_order_relaxed)) {
        // get an std::span fitting a single packet from each Ringbuf
        size_t recv_max_samples = max_num_samps;
        for (size_t n = 0; n < num_channels; n++) {
            auto span = ringbufs[n]->get_producer_span(max_num_samps * sample_size);
            buf_ptrs[n] = (void *) span.data();
            recv_max_samples = std::min(recv_max_samples, span.size_bytes() / sample_size);
        }

        // receive packet for each channel from USRP
        uhd::rx_metadata_t md;
        size_t samples = rx->recv(buf_ptrs, recv_max_samples, md, recv_timeout, true);

        // error handling
        if (samples == 0) {
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
                // TODO: implement zero padding to maintain continuous sample
                //       stream.
                logger->log("Sample stream continuity lost after overflow/out_of_sequence error.",
                            Log::ERROR);
                break;
            case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
            case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
            case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
            case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
            case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
                // presumably fatal errors. surrender and terminate the thread.
                // TODO: implement recovery by stopping and restarting the
                //       stream without causing an infinite restart deadlock.
                logger->log("Rx thread encountered irrecoverable error. Terminating.",
                            Log::FATAL);
                run.store(false, std::memory_order_relaxed);
                break;
            }

            continue;
        }

        // reduce timeout on first successful uhd::rx_streamer::recv()
        recv_timeout = 0.1;

        // process received samples
        for (size_t n = 0; n < num_channels; n++) {
            ringbufs[n]->produce(samples * sample_size);
        }
    }

    // stop streaming
    stream_cmd = uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
    rx->issue_stream_cmd(stream_cmd);
    logger->log_uhd_stream_cmd(stream_cmd);
    // TODO: receive until md.end_of_burst or timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

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
