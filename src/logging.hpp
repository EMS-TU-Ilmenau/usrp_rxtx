#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <tuple>
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
        auto to_string_ralign_color() const -> const std::string;

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

    class Uhd : public Base {
    public:
        Uhd(const uhd::log::logging_info& info)
            : Base{info.verbosity}
            , info{info}
        {};
        void serialize(std::ostream& console, std::ostream& logfile) const;

    protected:
        uhd::log::logging_info info;
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
        queue.push(Log::Uhd{info});
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
    std::atomic<bool> run {false};
    std::thread worker_handle;
    mpsc<std::variant<Log::Null,
                      Log::Misc,
                      Log::Exception,
                      Log::Exit,
                      Log::Uhd,
                      Log::UsrpChannels,
                      Log::UsrpHardware
                     >, 1024> queue;

    std::mutex mutex;
    std::condition_variable cond_var;

    std::ofstream log_file;

    void worker();
};

#endif /* LOGGING_HPP */
