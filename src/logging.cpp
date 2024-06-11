#include <cstdlib>

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

/// return error level formatted as right aligned std::string with ANSI color escapes
auto Log::Level::to_string_ralign_color() const -> const std::string
{
    switch (level) {
    case uhd::log::trace:
        return "\x1b[90mTRACE\x1b[0m";
    case uhd::log::debug:
        return "\x1b[90mDEBUG\x1b[0m";
    case uhd::log::info:
        return " \x1b[92mINFO\x1b[0m";
    case uhd::log::warning:
        return " \x1b[93mWARN\x1b[0m";
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
    auto days = std::chrono::floor<std::chrono::days>(time_point);
    std::chrono::year_month_day date {days};
    std::chrono::hh_mm_ss time {
        std::chrono::floor<std::chrono::nanoseconds>(time_point - days)
    };

    std::ostringstream buf;
    buf << std::setfill('0')
        << std::setw(4) << static_cast<int>(date.year())       << '-'
        << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
        << std::setw(2) << static_cast<unsigned>(date.day())   << 'T'
        << std::setw(2) << time.hours().count()                << ':'
        << std::setw(2) << time.minutes().count()              << ':'
        << std::setw(2) << time.seconds().count()              << '.'
        << std::setw(9) << time.subseconds().count()           << 'Z';

    return buf.str();
}

/// @brief convert embedded timestamp to short time-only string
auto Log::Base::time_short() const -> std::string
{
    auto days = std::chrono::floor<std::chrono::days>(time_point);
    std::chrono::hh_mm_ss time {
        std::chrono::floor<std::chrono::milliseconds>(time_point - days)
    };

    std::ostringstream buf;
    buf << std::setfill('0')
        << std::setw(2) << time.hours().count()                << ':'
        << std::setw(2) << time.minutes().count()              << ':'
        << std::setw(2) << time.seconds().count()              << '.'
        << std::setw(3) << time.subseconds().count()           << 'Z';

    return buf.str();
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
    console << time_short() << ' '
            << std::right << std::setw(5) << level.to_string_ralign_color() << ": "
            << msg << std::endl;

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", Json::Null{} },
        { "message", msg }
    }}.dump(&logfile);
    logfile << '\n';
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

    console << time_short() << ' '
            << std::right << std::setw(5) << level.to_string_ralign_color()
            << ": Exception: " << what << std::endl;

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "exception" },
        { "what", std::move(what) }
    }}.dump(&logfile);
    logfile << '\n';
}

void Log::Exit::serialize(std::ostream& console, std::ostream& logfile) const
{
    console << time_short() << ' '
            << std::right << std::setw(5) << level.to_string_ralign_color()
            << (exit_code == 0
                    ? ": Exiting successfully with exit code "
                    : ": Exiting abnormally with exit code ")
            << exit_code << '.' << std::endl;

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "exit" },
        { "exit_code", exit_code }
    }}.dump(&logfile);
    logfile << '\n';
}

void Log::UhdLogInfo::serialize(std::ostream& console, std::ostream& logfile) const
{
    if (info.verbosity >= uhd::log::info) {
        console << time_short() << ' '
                << std::right << std::setw(5) << level.to_string_ralign_color()
                << ": UHD/" << info.component << ": "
                << info.message << std::endl;
    }

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", time_epoch_ns() },
        { "_level", Log::Level{info.verbosity}.to_string() },
        { "_type", "uhd" },
        { "component", info.component },
        { "message", info.message }
    }}.dump(&logfile);
    logfile << '\n';
}

void Log::UhdRxMetadata::serialize(std::ostream& console, std::ostream& logfile) const
{
    Json::json_t error_code;
    switch (rx_meta.error_code) {
    case uhd::rx_metadata_t::ERROR_CODE_NONE:
        error_code = "ERROR_CODE_NONE";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
        error_code = "ERROR_CODE_LATE_COMMAND";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
        error_code = "ERROR_CODE_ALIGNMENT";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
        error_code = "ERROR_CODE_BAD_PACKET";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
        error_code = "ERROR_CODE_BROKEN_CHAIN";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
        error_code = "ERROR_CODE_OVERFLOW";
        break;
    case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
        error_code = "ERROR_CODE_TIMEOUT";
        break;
    default:
        error_code = (uint64_t) rx_meta.error_code;
    }

    Json::Object obj {{
        { "error_code", error_code },
        { "out_of_sequence", rx_meta.out_of_sequence },
        { "start_of_burst", rx_meta.start_of_burst },
        { "end_of_burst", rx_meta.end_of_burst },
        { "has_time_spec", rx_meta.has_time_spec },
        { "time_spec", (uint64_t) (rx_meta.time_spec.get_full_secs() * 1e9) +
                       (uint64_t) (rx_meta.time_spec.get_frac_secs() * 1e9) }
    }};

    console << time_short() << ' '
            << std::right << std::setw(5) << level.to_string_ralign_color()
            << ": rx_metadata_t: " << obj.dumps() << std::endl;

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
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
    }}.dump(&logfile);
    logfile << '\n';
}

void Log::UhdStreamCmd::serialize(std::ostream& console, std::ostream& logfile) const
{
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

    Json::Object obj {{
        { "stream_mode", stream_mode },
        { "num_samps", stream_cmd.num_samps },
        { "stream_now", stream_cmd.stream_now },
        { "time_spec", (uint64_t) (stream_cmd.time_spec.get_full_secs() * 1e9) +
                       (uint64_t) (stream_cmd.time_spec.get_frac_secs() * 1e9) }
    }};

    console << time_short() << ' '
            << std::right << std::setw(5) << level.to_string_ralign_color()
            << ": stream_cmd_t: " << obj.dumps() << std::endl;

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", time_epoch_ns() },
        { "_level", level.to_string() },
        { "_type", "rx_metadata" },
        { "stream_mode", stream_mode },
        { "num_samps", stream_cmd.num_samps },
        { "stream_now", stream_cmd.stream_now },
        { "time_spec", (uint64_t) (stream_cmd.time_spec.get_full_secs() * 1e9) +
                       (uint64_t) (stream_cmd.time_spec.get_frac_secs() * 1e9) }
    }}.dump(&logfile);
    logfile << '\n';
}

void Log::UsrpChannels::serialize(std::ostream& console, std::ostream& logfile) const
{
    // not logging to console
    (void) console;

    Json::Array chans_rx;
    int num_chan_rx = usrp->get_rx_num_channels();
    for (int chan = 0; chan < num_chan_rx; chan++) {
        chans_rx.emplace_back(Json::Object {{
            { "rate", usrp->get_rx_rate(chan) },
            { "gain", usrp->get_rx_gain(chan) }
        }});
    }

    Json::Array chans_tx;
    int num_chan_tx = usrp->get_tx_num_channels();
    for (int chan = 0; chan < num_chan_tx; chan++) {
        chans_tx.emplace_back(Json::Object {{
            { "rate", usrp->get_rx_rate(chan) },
            { "gain", usrp->get_rx_gain(chan) }
        }});
    }

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", time_epoch_ns() },
        { "_level", Log::INFO.to_string() },
        { "_type", "channels" },
        { "rx", std::move(chans_rx) },
        { "tx", std::move(chans_tx) },
    }}.dump(&logfile);
    logfile << '\n';
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

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", time_epoch_ns() },
        { "_level", Log::INFO.to_string() },
        { "_type", "hardware" },
        { "motherboards", std::move(info_mboard) },
        { "daughterboards_rx", std::move(info_rx) },
        { "daughterboards_tx", std::move(info_tx) },
    }}.dump(&logfile);
    logfile << '\n';
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
    Json::Object{{
        { "_level", Log::DEBUG.to_string() },
        { "_type", "software" },
        { "git_origin", std::string{git_origin} },
        { "git_hash", std::string{git_hash} }
    }}.dump(&log_file);
    log_file << '\n';

    // log hostname and uname
    Json::Object{{
        { "_level", Log::DEBUG.to_string() },
        { "_type", "host" },
        { "hostname", std::string{hostname} },
        { "uname", unamebuf.str() }
    }}.dump(&log_file);
    log_file << '\n';

    // log contents of loaded configuration file
    Json::Object{{
        { "_level", Log::DEBUG.to_string() },
        { "_type", "config" },
        { "config", config }
    }}.dump(&log_file);
    log_file << '\n';

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
