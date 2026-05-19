// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#include <cstdlib>
#include <format>

extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
    #include <sys/utsname.h>
    #include <unistd.h>
}

#include "error.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "version.hpp"

/// convert uhd::time_spec_t to HH:MM:SS timestamp
static inline auto timespec_to_str(const uhd::time_spec_t& time) -> const std::string
{
    using namespace std::chrono;
    using ns_clock = std::chrono::time_point<system_clock, nanoseconds>;

    uint64_t nsec = (uint64_t) (time.get_full_secs() * 1e9) +
                    (uint64_t) (time.get_frac_secs() * 1e9);
    //return std::format("{:%Y-%m-%dT%H:%M:%S}Z", ns_clock{nanoseconds{nsec}});
    return std::format("{:%H:%M:%S}Z", ns_clock{nanoseconds{nsec}});
}

/// return error level formatted as std::string
auto Log::Level::to_string() const -> const std::string
{
    switch (level) {
    case uhd::log::trace:
        return "TRACE";
    case uhd::log::debug:
        return "DEBUG";
    case uhd::log::info:
        return "INFO";
    case uhd::log::warning:
        return "WARN";
    case uhd::log::error:
        return "ERROR";
    case uhd::log::fatal:
        return "FATAL";
    case uhd::log::off:
        return "OFF";
    default:
        return "INVAL";
    }
}

/// return error level formatted as std::string with ANSI color escapes
auto Log::Level::to_string_color() const -> const std::string
{
    switch (level) {
    case uhd::log::trace:
        return "\x1b[90mTRACE\x1b[0m";
    case uhd::log::debug:
        return "\x1b[90mDEBUG\x1b[0m";
    case uhd::log::info:
        return "\x1b[92mINFO\x1b[0m";
    case uhd::log::warning:
        return "\x1b[93mWARN\x1b[0m";
    case uhd::log::error:
        return "\x1b[91mERROR\x1b[0m";
    case uhd::log::fatal:
        return "\x1b[95mFATAL\x1b[0m";
    case uhd::log::off:
        return "OFF";
    default:
        return "INVAL";
    }
}

/// return error level formatted as fixed width std::string with ANSI color escapes
auto Log::Level::to_string_color_fixed() const -> const std::string
{
    switch (level) {
    case uhd::log::trace:
        return "\x1b[90mTRACE\x1b[0m";
    case uhd::log::debug:
        return "\x1b[90mDEBUG\x1b[0m";
    case uhd::log::info:
        return "\x1b[92mINFO\x1b[0m ";
    case uhd::log::warning:
        return "\x1b[93mWARN\x1b[0m ";
    case uhd::log::error:
        return "\x1b[91mERROR\x1b[0m";
    case uhd::log::fatal:
        return "\x1b[95mFATAL\x1b[0m";
    case uhd::log::off:
        return "OFF";
    default:
        return "INVAL";
    }
}

/// @brief convert embedded timestamp to RFC3339-formatted string
auto Log::Base::time_rfc3339() const -> std::string
{
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", time_point);
}

/// @brief convert embedded timestamp to short time-only string
auto Log::Base::time_short() const -> std::string
{
    auto truncated = std::chrono::floor<std::chrono::microseconds>(time_point);
    return std::format("{:%H:%M:%S}Z", truncated);
}

/// @brief convert embedded timestamp to nanoseconds since POSIX epoch
auto Log::Base::time_epoch_ns() const -> uint64_t
{
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(
        time_point).time_since_epoch().count();
}

void Log::Null::serialize(std::ostream& console, std::ostream& logfile) const
{
    (void) console;
    (void) logfile;
}

void Log::Misc::serialize(std::ostream& console, std::ostream& logfile) const
{
    console << time_short() << ' ' << level.to_string_color_fixed() << ": "
            << msg << std::endl;

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", Json::Null{} },
        { "message", msg }
    }} << '\n';
}

void Log::Exception::serialize(std::ostream& console, std::ostream& logfile) const
{
    std::string what;

    try {
         std::rethrow_exception(eptr);
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
        what = "Unknown exception";
    }

    console << time_short() << ' ' << level.to_string_color_fixed()
            << ": Exception: " << what << std::endl;

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "exception" },
        { "what", std::move(what) }
    }} << '\n';
}

void Log::Exit::serialize(std::ostream& console, std::ostream& logfile) const
{
    console << time_short() << ' ' << level.to_string_color_fixed()
            << (exit_code == 0
                    ? ": Exiting successfully with exit code "
                    : ": Exiting abnormally with exit code ")
            << exit_code << '.' << std::endl;

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "exit" },
        { "exit_code", exit_code }
    }} << '\n';
}

void Log::RxZeropad::serialize(std::ostream& console, std::ostream& logfile) const
{
    console << time_short() << ' ' << level.to_string_color_fixed()
            << ": Zero padding " << samples
            << " samples at ring buffer offset " << offset << " samples."
            << std::endl;

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "rx_zeropad" },
        { "offset", offset },
        { "samples", samples }
    }} << '\n';
}

void Log::UhdLogInfo::serialize(std::ostream& console, std::ostream& logfile) const
{
    if (info.verbosity >= uhd::log::info) {
        console << time_short() << ' ' << level.to_string_color_fixed()
                << ": UHD/" << info.component << ": "
                << info.message << std::endl;
    }

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", Log::Level{info.verbosity}.to_string() },
        { "_type", "uhd" },
        { "component", info.component },
        { "message", info.message }
    }} << '\n';
}

void Log::UhdAsyncMetadata::serialize(std::ostream& console, std::ostream& logfile) const
{
    std::string event;
    Json::json_t event_code;
    switch (async_meta.event_code) {
    case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
        event = "BURST_ACK";
        event_code = "EVENT_CODE_BURST_ACK";
        break;
    case uhd::async_metadata_t::EVENT_CODE_USER_PAYLOAD:
        event = "USER_PAYLOAD";
        event_code = "EVENT_CODE_USER_PAYLOAD";
        break;
    case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR:
        event = "SEQ_ERROR";
        event_code = "EVENT_CODE_SEQ_ERROR";
        break;
    case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST:
        event = "SEQ_ERROR_IN_BURST";
        event_code = "EVENT_CODE_SEQ_ERROR_IN_BURST";
        break;
    case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
        event = "TIME_ERROR";
        event_code = "EVENT_CODE_TIME_ERROR";
        break;
    case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
        event = "UNDERFLOW";
        event_code = "EVENT_CODE_UNDERFLOW";
        break;
    case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
        event = "UNDERFLOW_IN_PACKET";
        event_code = "EVENT_CODE_UNDERFLOW_IN_PACKET";
        break;
    default:
        event = std::to_string(async_meta.event_code);
        event_code = (uint64_t) async_meta.event_code;
    }

    console << time_short() << ' ' << level.to_string_color_fixed()
            << ": Tx async event " << event << " occurred "
            << "on channel " << async_meta.channel
            << (async_meta.has_time_spec ? " at " : "")
            << (async_meta.has_time_spec ? timespec_to_str(async_meta.time_spec) : "")
            << '.' << std::endl;

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "async_metadata" },
        { "event_code", event_code },
        { "channel", async_meta.channel },
        { "has_time_spec", async_meta.has_time_spec },
        { "time_spec", (uint64_t) (async_meta.time_spec.get_full_secs() * 1e9) +
                       (uint64_t) (async_meta.time_spec.get_frac_secs() * 1e9) },
        { "user_payload", Json::Array{{
            (uint64_t) async_meta.user_payload[0], (uint64_t) async_meta.user_payload[1],
            (uint64_t) async_meta.user_payload[2], (uint64_t) async_meta.user_payload[3]
        }}}
    }} << '\n';
}

void Log::UhdRxMetadata::serialize(std::ostream& console, std::ostream& logfile) const
{
    std::string error;
    Json::json_t error_code;
    switch (rx_meta.error_code) {
    case uhd::rx_metadata_t::ERROR_CODE_NONE:
        error = "NONE";
        error_code = "ERROR_CODE_NONE";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
        error = "LATE_COMMAND";
        error_code = "ERROR_CODE_LATE_COMMAND";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
        error = "ALIGNMENT";
        error_code = "ERROR_CODE_ALIGNMENT";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
        error = "BAD_PACKET";
        error_code = "ERROR_CODE_BAD_PACKET";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
        error = "BROKEN_CHAIN";
        error_code = "ERROR_CODE_BROKEN_CHAIN";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
        error = "OVERFLOW";
        error_code = "ERROR_CODE_OVERFLOW";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
        error = "TIMEOUT";
        error_code = "ERROR_CODE_TIMEOUT";
        break;
    default:
        error = std::to_string(rx_meta.error_code);
        error_code = (uint64_t) rx_meta.error_code;
    }

    if (rx_meta.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE) {
        console << time_short() << ' ' << level.to_string_color_fixed()
                << ": Rx stream "
                << (rx_meta.end_of_burst ? "ended" : "started")
                << (rx_meta.has_time_spec ? " at " : "")
                << (rx_meta.has_time_spec ? timespec_to_str(rx_meta.time_spec) : "")
                << '.' << std::endl;
    } else {
        console << time_short() << ' ' << level.to_string_color_fixed()
                << "Rx stream error " << error << " occurred"
                << (rx_meta.has_time_spec ? " at " : "")
                << (rx_meta.has_time_spec ? timespec_to_str(rx_meta.time_spec) : "")
                << '.' << std::endl;
    }

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "rx_metadata" },
        { "error_code", error_code },
        { "out_of_sequence", rx_meta.out_of_sequence },
        { "start_of_burst", rx_meta.start_of_burst },
        { "end_of_burst", rx_meta.end_of_burst },
        { "has_time_spec", rx_meta.has_time_spec },
        { "time_spec", (uint64_t) (rx_meta.time_spec.get_full_secs() * 1e9) +
                       (uint64_t) (rx_meta.time_spec.get_frac_secs() * 1e9) }
    }} << '\n';
}

void Log::UhdStreamCmd::serialize(std::ostream& console, std::ostream& logfile) const
{
    console << time_short() << ' ' << level.to_string_color_fixed();
    switch (stream_cmd.stream_mode) {
    case uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
        console << ": Rx stream start ";
        break;
    case uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS:
        console << ": Rx stream stop ";
        break;
    case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE:
        console << ": Rx stream " << stream_cmd.num_samps << " samples ";
        break;
    case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE:
        console << ": Rx stream " << stream_cmd.num_samps << " samples and more ";
        break;
    default:
        throw not_implemented_error{"unknown uhd::stream_cmd_t value: "
            + std::to_string(stream_cmd.stream_mode)};
    }

    if (stream_cmd.stream_now) {
        console << "now." << std::endl;
    } else {
        console << "scheduled at " << timespec_to_str(stream_cmd.time_spec) << '.'
                << std::endl;
    }

    Json::json_t stream_mode;
    switch (stream_cmd.stream_mode) {
    case uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
        stream_mode = "STREAM_MODE_START_CONTINUOUS";
        break;
    case uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS:
        stream_mode = "STREAM_MODE_STOP_CONTINUOUS";
        break;
    case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE:
        stream_mode = "STREAM_MODE_NUM_SAMPS_AND_DONE";
        break;
    case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE:
        stream_mode = "STREAM_MODE_NUM_SAMPS_AND_MORE";
        break;
    default:
        stream_mode = (uint64_t) stream_cmd.stream_mode;
        break;
    }

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "rx_stream_cmd" },
        { "stream_mode", stream_mode },
        { "num_samps", stream_cmd.num_samps },
        { "stream_now", stream_cmd.stream_now },
        { "time_spec", (uint64_t) (stream_cmd.time_spec.get_full_secs() * 1e9) +
                       (uint64_t) (stream_cmd.time_spec.get_frac_secs() * 1e9) }
    }} << '\n';
}

void Log::UhdTuneResult::serialize(std::ostream& console, std::ostream& logfile) const
{
    Json::Object obj {{
        { "clipped_rf_freq", tune_res.clipped_rf_freq },
        { "target_rf_freq", tune_res.target_rf_freq },
        { "actual_rf_freq", tune_res.actual_rf_freq },
        { "target_dsp_freq", tune_res.target_dsp_freq },
        { "actual_dsp_freq", tune_res.actual_dsp_freq }
    }};

    console << time_short() << ' ' << level.to_string_color_fixed()
            << (path == Path::RX ? ": Tuned Rx channel " : ": Tuned Tx channel ") << chan
            << ": " << obj << std::endl;

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "tune_result" },
        { "path", path == Path::RX ? "rx" : "tx" },
        { "channel", chan },
        { "clipped_rf_freq", tune_res.clipped_rf_freq },
        { "target_rf_freq", tune_res.target_rf_freq },
        { "actual_rf_freq", tune_res.actual_rf_freq },
        { "target_dsp_freq", tune_res.target_dsp_freq },
        { "actual_dsp_freq", tune_res.actual_dsp_freq }
    }} << '\n';
}

void Log::UhdTxMetadata::serialize(std::ostream& console, std::ostream& logfile) const
{
    // only log start and end of burst to console
    if (tx_meta.start_of_burst || tx_meta.end_of_burst) {
        console << time_short() << ' ' << level.to_string_color_fixed()
                << (tx_meta.start_of_burst ? ": Tx burst start " : ": Tx burst end ")
                << (tx_meta.has_time_spec ? "at " : "now")
                << (tx_meta.has_time_spec ? timespec_to_str(tx_meta.time_spec) : "")
                << '.' << std::endl;
    }

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "tx_metadata" },
        { "start_of_burst", tx_meta.start_of_burst },
        { "end_of_burst", tx_meta.end_of_burst },
        { "has_time_spec", tx_meta.has_time_spec },
        { "time_spec", (uint64_t) (tx_meta.time_spec.get_full_secs() * 1e9) +
                       (uint64_t) (tx_meta.time_spec.get_frac_secs() * 1e9) }
    }} << '\n';
}

void Log::UsrpChannels::serialize(std::ostream& console, std::ostream& logfile) const
{
    // not logging to console
    (void) console;

    Json::Array chans_rx;
    for (size_t chan = 0; chan < usrp->get_rx_num_channels(); chan++) {
        chans_rx.emplace_back(Json::Object{{
            { "antenna", usrp->get_rx_antenna(chan) },
            { "bandwidth", usrp->get_rx_bandwidth(chan) },
            { "frequency", usrp->get_rx_freq(chan) },
            { "gain", usrp->get_rx_gain(chan) },
            { "lo_source", usrp->get_rx_lo_source(uhd::usrp::multi_usrp::ALL_LOS, chan) },
            { "rate", usrp->get_rx_rate(chan) }
        }});
    }

    Json::Array chans_tx;
    for (size_t chan = 0; chan < usrp->get_tx_num_channels(); chan++) {
        chans_tx.emplace_back(Json::Object{{
            { "antenna", usrp->get_tx_antenna(chan) },
            { "bandwidth", usrp->get_tx_bandwidth(chan) },
            { "frequency", usrp->get_rx_freq(chan) },
            { "gain", usrp->get_tx_gain(chan) },
            { "lo_source", usrp->get_tx_lo_source(uhd::usrp::multi_usrp::ALL_LOS, chan) },
            { "rate", usrp->get_tx_rate(chan) }
        }});
    }

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", Log::INFO.to_string() },
        { "_type", "channels" },
        { "rx", std::move(chans_rx) },
        { "tx", std::move(chans_tx) },
    }} << '\n';
}

void Log::UsrpHardware::serialize(std::ostream& console, std::ostream& logfile) const
{
    // not logging to console
    (void) console;

    // get property tree
    uhd::property_tree::sptr tree = usrp->get_device()->get_tree();

    // get motherboard infos
    Json::Array info_mboard;
    for (const std::string& mb_num : tree->list("/mboards")) {
        uhd::usrp::mboard_eeprom_t mb_eeprom =
            tree->access<uhd::usrp::mboard_eeprom_t>(
                "/mboards/" + mb_num + "/eeprom").get();

        Json::Object info;
        for (const std::string& key : mb_eeprom.keys()) {
            info.emplace_back(key, mb_eeprom[key]);
        }
        info_mboard.emplace_back(info);
    }

    // get infos about RX channels
    Json::Array info_rx;
    int num_chan_rx = usrp->get_rx_num_channels();
    for (int chan = 0; chan < num_chan_rx; chan++) {
        Json::Object info;
        for (const std::string& key : usrp->get_usrp_rx_info(chan).keys()) {
            info.emplace_back(key, usrp->get_usrp_rx_info(chan)[key]);
        }
        info_rx.emplace_back(info);
    }

    // get infos about TX channels
    Json::Array info_tx;
    int num_chan_tx = usrp->get_tx_num_channels();
    for (int chan = 0; chan < num_chan_tx; chan++) {
        Json::Object info;
        for (const std::string& key : usrp->get_usrp_tx_info(chan).keys()) {
            info.emplace_back(key, usrp->get_usrp_tx_info(chan)[key]);
        }
        info_tx.emplace_back(info);
    }

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", Log::INFO.to_string() },
        { "_type", "hardware" },
        { "motherboards", std::move(info_mboard) },
        { "daughterboards_rx", std::move(info_rx) },
        { "daughterboards_tx", std::move(info_tx) },
    }} << '\n';
}

void Log::WrOpen::serialize(std::ostream& console, std::ostream& logfile) const
{
    using namespace std::chrono;
    using ns_clock = std::chrono::time_point<system_clock, nanoseconds>;

    console << time_short() << ' ' << level.to_string_color_fixed()
            << ": Writing file " << std::quoted(path.generic_string())
            << " from ring buffer offset " << offset << " samples."
            << std::endl;

    logfile << Json::Object{{
        { "_time", time_rfc3339() },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "wr_open" },
        { "file", path.generic_string() },
        { "time_start", std::format("{:%Y-%m-%dT%H:%M:%S}Z",
            ns_clock{nanoseconds{start_ns}})
        },
        { "time_start_ns", start_ns },
        { "offset_samples", offset }
    }} << '\n';
}

Logger::Logger(const std::filesystem::path& path_prefix, Json::Object&& config)
{
    // UHD has a special logging fastpath for urgent log messages that bypass
    // the regular logging settings. it is used for logging the UOSDL letters
    // that indicate stream errors. these however clobbers our Logger's output.
    // as Logger already logs these stream errors, disable UHD's fastpath.
    // https://kb.ettus.com/The_UHD_logging_facility
    // https://files.ettus.com/manual/page_general.html#general_ounotes
    // https://files.ettus.com/manual/page_usrp_x3x0_config.html#x3x0cfg_hosthw_troubleshooting
#if _POSIX_C_SOURCE >= 200112L
    setenv("UHD_LOG_FASTPATH_DISABLE", "ON", 1);
#endif

    // get hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        throw syscall_error{"gethostname() failed"};

    // get uname
    struct utsname utsbuf;
    std::ostringstream unamebuf;
    if (uname(&utsbuf) != 0)
        throw syscall_error{"uname() failed"};
    unamebuf << utsbuf.sysname << ' '
             << utsbuf.nodename << ' '
             << utsbuf.release << ' '
             << utsbuf.version << ' '
             << utsbuf.machine;

    // determine log file timestamp
    uint64_t epoch_sec = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    // open log file
    std::filesystem::path path = path_prefix;
    path += "_" + std::to_string(epoch_sec) + ".log";
    log_file.open(path);
    if (!log_file)
        throw syscall_error{"error opening log file: " + path.generic_string()};

    // log software origin and commit hash
    log_file << Json::Object{{
        { "_level", Log::DEBUG.to_string() },
        { "_type", "software" },
        { "git_origin", std::string{git_origin} },
        { "git_hash", std::string{git_hash} }
    }} << '\n';

    // log hostname and uname
    log_file << Json::Object{{
        { "_level", Log::DEBUG.to_string() },
        { "_type", "host" },
        { "hostname", std::string{hostname} },
        { "uname", unamebuf.str() }
    }} << '\n';

    // log contents of loaded configuration file
    log_file << Json::Object{{
        { "_level", Log::DEBUG.to_string() },
        { "_type", "config" },
        { "config", config }
    }} << '\n';

    // spawn worker
    worker_handle = std::thread{&Logger::worker, this};
}

/// @brief Stop logging worker thread.
/// @warning Will block until log queue is empty. Only destroy after all other
///          log producing threads have been terminated.
Logger::~Logger()
{
    // stop and join the worker thread
    run.store(false, std::memory_order_relaxed);
    worker_handle.join();

    // serialize all remaining messages left in log queue
    serialize_messages();
}

void Logger::worker()
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "logging");
#endif

    // producer-side notification does not lock the mutex, so we can hold the
    // lock continuously
    std::unique_lock lock {mutex};

    while (run.load(std::memory_order_relaxed)) {
        // producer will notify us once per push into the log queue, but as it
        // does so without holding the lock, we may miss the notification
        // subject to a race condition. hence, periodically poll log queue for
        // new entries by letting wait time out frequently.
        cond_var.wait_for(lock, std::chrono::milliseconds{50});

        // TODO: check for lost log messages

        // serialize all currently queued messages (drains queue)
        // NOTE: will block destructor until queue is empty, so may cause
        //       deadlock if another thread keeps logging faster than we
        //       consume here.
        serialize_messages();
    }
} catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << '\n'
              << "Exception occurred in logging thread. Log messages may have been lost. Thread terminated."
              << std::endl;
    run.store(false, std::memory_order_relaxed);
} catch (...) {
    std::cerr << "Unknown exception occurred in logging thread. Log messages may have been lost. Thread terminated."
              << std::endl;
    run.store(false, std::memory_order_relaxed);
}

/// serialize all currently queued log messages
void Logger::serialize_messages()
{
    for (auto opt = queue.pop(); opt.has_value(); opt = queue.pop()) {
        // defer formatting to individual variant's .serialize() function
        std::ostringstream buf;
        std::visit([&buf, this](auto& arg){
            // FIXME: Log::*::serialize() prints ANSI escape codes to
            //        colorize log levels without verifying that console
            //        has color support
            // TODO: check environment variable TERM ends with "color"
            return arg.serialize(buf, log_file);
        }, opt.value());

        // TODO: coalesce terminal output of multiple consecutive messages
        //       without accumulating too many messages in case of bursts.
        std::cerr << buf.str();
    }
}
