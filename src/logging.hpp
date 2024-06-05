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

#include "mpsc.hpp"

namespace Log {
    class Base {
    public:
        Base(const std::source_location src_loc = std::source_location::current())
            : src_loc{src_loc}
        {};

        auto time_rfc3339() const -> std::string;
        auto time_epoch_ns() const -> uint64_t;
        auto prio_str() const -> const std::string;

    protected:
        std::source_location src_loc;
        std::chrono::time_point<std::chrono::system_clock> time_point
            {std::chrono::system_clock::now()};
    };

    class Misc : public Base {
    public:
        Misc(std::string&& msg = "",
            const std::source_location src_loc
                = std::source_location::current())
            : Base{src_loc}
            , msg{std::move(msg)}
        {};
        void serialize(std::ostream& log_stream, std::ostream& term_stream) const;

    private:
        std::string msg;
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

    inline void log(std::string&& mesg)
    {
        queue.push(Log::Misc{std::move(mesg)});
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
    }

private:
    std::atomic<bool> run {false};
    std::thread worker_handle;
    mpsc<std::variant<Log::Misc>, 128> queue;

    std::mutex mutex;
    std::condition_variable cond_var;

    std::ofstream log_file;

    void worker();
};

#endif /* LOGGING_HPP */
