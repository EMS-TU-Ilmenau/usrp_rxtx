// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>
#include <uhd/types/tune_request.hpp>
#include <uhd/types/tune_result.hpp>
#include <variant>

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/log.hpp>

#include "json.hpp"
#include "mpsc.hpp"

namespace Log {
    /// convenience wrapper around error levels enum used by UHD
    /// @see https://files.ettus.com/manual/namespaceuhd_1_1log.html
    class Level {
    public:
        Level(uhd::log::severity_level lvl)
            : level{lvl}
        {};

        /// return error level formatted as std::string
        auto to_string() const -> const std::string;

        /// return error level formatted as std::string with ANSI color escapes
        auto to_string_color() const -> const std::string;

        /// return error level formatted as right aligned std::string with ANSI color escapes
        auto to_string_color_fixed() const -> const std::string;

    protected:
        uhd::log::severity_level level;
    };

    const Level TRACE {uhd::log::trace};
    const Level DEBUG {uhd::log::debug};
    const Level INFO  {uhd::log::info};
    const Level WARN  {uhd::log::warning};
    const Level ERROR {uhd::log::error};
    const Level FATAL {uhd::log::fatal};

    /// placeholder log entry for default initialization of MPSC queue
    class Null {
    public:
        void serialize(std::ostream& console, std::ostream& logfile) const;
    };

    class Base {
    public:
        Base(Level level)
            : level{level}
        {};

        auto time_epoch_ns() const -> uint64_t;
        auto time_rfc3339() const -> std::string;
        auto time_short() const -> std::string;

    protected:
        Level level;
        std::source_location src_loc;
        std::chrono::time_point<std::chrono::system_clock> time_point
            {std::chrono::system_clock::now()};
    };

    class Misc : public Base {
    public:
        Misc(std::string&& msg = "", Level level = INFO)
            : Base{level}
            , msg{std::move(msg)}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        std::string msg;
    };

    class Exception : public Base {
    public:
        Exception(std::exception_ptr&& eptr)
            : Base{ERROR}
            , eptr{std::move(eptr)}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        std::exception_ptr eptr;
    };

    class Exit : public Base {
    public:
        Exit(int exit_code)
            : Base{exit_code == 0 ? INFO : FATAL}
            , exit_code{exit_code}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        int exit_code;
    };

    class RxZeropad : public Base {
    public:
        RxZeropad(uint64_t offset, uint64_t samples)
            : Base{offset == 0 ? DEBUG : WARN}
            , offset{offset}
            , samples{samples}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uint64_t offset;
        uint64_t samples;
    };

    class UhdLogInfo : public Base {
    public:
        UhdLogInfo(const uhd::log::logging_info& info)
            : Base{info.verbosity}
            , info{info}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::log::logging_info info;
    };

    class UhdAsyncMetadata : public Base {
    public:
        UhdAsyncMetadata(const uhd::async_metadata_t& async_meta)
            : Base{async_meta.event_code == uhd::async_metadata_t::EVENT_CODE_BURST_ACK ||
                   async_meta.event_code == uhd::async_metadata_t::EVENT_CODE_USER_PAYLOAD
                   ? DEBUG : ERROR}
            , async_meta{async_meta}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::async_metadata_t async_meta;
    };

    class UhdRxMetadata : public Base {
    public:
        UhdRxMetadata(const uhd::rx_metadata_t& rx_meta)
            : Base{rx_meta.error_code == 0 ? DEBUG : ERROR}
            , rx_meta{rx_meta}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::rx_metadata_t rx_meta;
    };

    class UhdStreamCmd : public Base {
    public:
        UhdStreamCmd(const uhd::stream_cmd_t& stream_cmd)
            : Base{INFO}
            , stream_cmd{stream_cmd}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::stream_cmd_t stream_cmd;
    };

    class UhdTuneResult : public Base {
    public:
        enum Path { RX, TX };

        UhdTuneResult(const uhd::tune_result_t& tune_res, Path path, size_t chan)
            : Base{tune_res.clipped_rf_freq == tune_res.target_rf_freq &&
                   tune_res.actual_rf_freq  == tune_res.target_rf_freq &&
                   tune_res.actual_dsp_freq == tune_res.target_dsp_freq
                   ? INFO : ERROR}
            , tune_res{tune_res}
            , path{path}
            , chan{chan}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::tune_result_t tune_res;
        Path path;
        size_t chan;
    };

    class UhdTxMetadata : public Base {
    public:
        UhdTxMetadata(const uhd::tx_metadata_t& tx_meta)
            : Base{tx_meta.start_of_burst || tx_meta.end_of_burst ? INFO : DEBUG}
            , tx_meta{tx_meta}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::tx_metadata_t tx_meta;
    };

    class UsrpHardware : public Base {
    public:
        UsrpHardware(uhd::usrp::multi_usrp::sptr usrp)
            : Base{INFO}
            , usrp{usrp}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::usrp::multi_usrp::sptr usrp;
    };

    class UsrpChannels : public Base {
    public:
        UsrpChannels(uhd::usrp::multi_usrp::sptr usrp)
            : Base{INFO}
            , usrp{usrp}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::usrp::multi_usrp::sptr usrp;
    };

    class WrOpen : public Base {
    public:
        WrOpen(const std::filesystem::path& path, const uint64_t start_ns, const uint64_t offset)
            : Base{INFO}
            , path{path}
            , start_ns{start_ns}
            , offset{offset}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        std::filesystem::path path;
        uint64_t start_ns;
        uint64_t offset;
    };
}

class Logger {
public:
    using sptr = std::shared_ptr<Logger>;

    Logger(const std::filesystem::path& path_prefix, Json::Object&& config);
    ~Logger();

    // delete copy constructor and copy assignment operator
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    auto is_running() const -> bool
    {
        return run.load(std::memory_order_relaxed);
    };

    void log(std::string&& mesg, Log::Level level = Log::INFO)
    {
        queue.push(Log::Misc{std::move(mesg), level});
        // notify consumer without holding the mutex to avoid blocking.
        // NOTE: subject to race condition, which may leave one (or more?)
        //       messages unconsumed until next notification or consumer
        //       timeout.
        // NOTE: notifying after each queue push is somewhat optimized for high
        //       logging load. while the worker thread is active, i.e., not
        //       waiting to be notified, each notify_one() should incur no
        //       syscall.
        // TODO: reduce logging overhead by not notifying while the worker
        //       is active. must be race safe wrt sudden logging bursts.
        cond_var.notify_one();
    };

    void log_exception(std::exception_ptr&& exc)
    {
        queue.push(Log::Exception{std::move(exc)});
        cond_var.notify_one();
    };

    void log_exit(int exit_code) {
        queue.push(Log::Exit{exit_code});
        cond_var.notify_one();
    };

    void log_uhd(const uhd::log::logging_info& info)
    {
        queue.push(Log::UhdLogInfo{info});
        cond_var.notify_one();
    };

    void log_uhd_async_metadata(const uhd::async_metadata_t& async_meta)
    {
        queue.push(Log::UhdAsyncMetadata{async_meta});
        cond_var.notify_one();
    };

    void log_uhd_rx_metadata(const uhd::rx_metadata_t& rx_meta)
    {
        queue.push(Log::UhdRxMetadata{rx_meta});
        cond_var.notify_one();
    };

    void log_rx_zeropad(uint64_t offset, uint64_t samples)
    {
        queue.push(Log::RxZeropad{offset, samples});
        cond_var.notify_one();
    };

    void log_uhd_stream_cmd(const uhd::stream_cmd_t& stream_cmd)
    {
        queue.push(Log::UhdStreamCmd{stream_cmd});
        cond_var.notify_one();
    };

    void log_uhd_tune_result_rx(const uhd::tune_result_t& tune_res, size_t chan)
    {
        queue.push(Log::UhdTuneResult{tune_res, Log::UhdTuneResult::RX, chan});
        cond_var.notify_one();
    };

    void log_uhd_tune_result_tx(const uhd::tune_result_t& tune_res, size_t chan)
    {
        queue.push(Log::UhdTuneResult{tune_res, Log::UhdTuneResult::TX, chan});
        cond_var.notify_one();
    };

    void log_uhd_tx_metadata(const uhd::tx_metadata_t& tx_meta)
    {
        queue.push(Log::UhdTxMetadata{tx_meta});
        cond_var.notify_one();
    };

    void log_wr_open(const std::filesystem::path& path, const uint64_t start_ns, const uint64_t offset)
    {
        queue.push(Log::WrOpen{path, start_ns, offset});
        cond_var.notify_one();
    };

    void log_usrp_channels(uhd::usrp::multi_usrp::sptr usrp)
    {
        queue.push(Log::UsrpChannels{std::move(usrp)});
        cond_var.notify_one();
    };

    void log_usrp_hardware(uhd::usrp::multi_usrp::sptr usrp)
    {
        queue.push(Log::UsrpHardware{std::move(usrp)});
        cond_var.notify_one();
    };

private:
    std::atomic<bool> run {true};
    std::thread worker_handle;

    mpsc<std::variant<Log::Null,
                      Log::Misc,
                      Log::Exception,
                      Log::Exit,
                      Log::RxZeropad,
                      Log::UhdLogInfo,
                      Log::UhdAsyncMetadata,
                      Log::UhdRxMetadata,
                      Log::UhdStreamCmd,
                      Log::UhdTuneResult,
                      Log::UhdTxMetadata,
                      Log::UsrpChannels,
                      Log::UsrpHardware,
                      Log::WrOpen
                     >, 1024> queue;

    std::mutex mutex;
    std::condition_variable cond_var;

    std::ofstream log_file;

    void worker();
    void serialize_messages();
};

#endif /* LOGGING_HPP */
