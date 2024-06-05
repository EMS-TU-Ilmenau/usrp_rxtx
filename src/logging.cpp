#include "error.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "version.hpp"

extern "C" {
    #include <sys/utsname.h>
    #include <unistd.h>
}

Logger::Logger(const std::filesystem::path& path_prefix, Json::Object&& config)
{
    // get hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        throw syscall_error{"gethostbyname() failed"};

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
        { "_type", "software" },
        { "git_origin", std::string{git_origin} },
        { "git_hash", std::string{git_hash} }
    }}.dump(&log_file);
    log_file << '\n';

    // log hostname and uname
    Json::Object{{
        { "_type", "host" },
        { "hostname", std::string{hostname} },
        { "uname", unamebuf.str() }
    }}.dump(&log_file);
    log_file << '\n';

    // log contents of loaded configuration file
    Json::Object{{
        { "_type", "config" },
        { "config", config }
    }}.dump(&log_file);
    log_file << '\n';

    // spawn and wait for worker
    worker_handle = std::thread{&Logger::worker, this};
    run.wait(false);
}

/// @brief Stop logging worker thread.
/// @warning Will block until log queue is empty. Only destroy after all other
///          log producing threads have been terminated.
Logger::~Logger()
{
    if (run.exchange(false, std::memory_order_relaxed))
        worker_handle.join();
    log_file.close();
}

void Logger::worker()
{
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "logging");
#endif

    // notify constructor
    run.store(true, std::memory_order_seq_cst);
    run.notify_one();

    // producers-side notification does not lock the mutex, so we can hold the
    // lock continuously
    std::unique_lock lock {mutex};

    while (run.load(std::memory_order_relaxed)) {
        // producer will notify us once per push into the log queue, but as it
        // does so without holding the lock, we may miss the notification
        // subject to a race condition. hence, periodically poll log queue for
        // new entries by letting wait time out frequently.
        cond_var.wait_for(lock, std::chrono::milliseconds{50});

        // drain queue
        // NOTE: will block destructor until queue is empty, so may cause
        //       deadlock if another thread keeps logging faster than we
        //       consume here.
        for (auto opt = queue.pop(); opt.has_value(); opt = queue.pop()) {
            // defer formatting to individual variant's .serialize() function
            // TODO: pass std::ostringstream instead of std::cout to coalesce
            //       terminal output
            std::visit([this](auto& arg){ return arg.serialize(log_file, std::cout); }, opt.value());
        }
    }
}

/// @brief convert embedded timestamp to RFC3339-formatted string
auto Log::Base::time_rfc3339() const -> std::string
{
    auto days = std::chrono::floor<std::chrono::days>(time_point);
    std::chrono::year_month_day date{days};
    std::chrono::hh_mm_ss time{
        std::chrono::floor<std::chrono::nanoseconds>(time_point - days)
    };

    std::ostringstream rfc3339;
    rfc3339 << std::setfill('0')
            << std::setw(4) << static_cast<int>(date.year())       << '-'
            << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
            << std::setw(2) << static_cast<unsigned>(date.day())   << 'T'
            << std::setw(2) << time.hours().count()                << ':'
            << std::setw(2) << time.minutes().count()              << ':'
            << std::setw(2) << time.seconds().count()              << '.'
            << std::setw(9) << time.subseconds().count()           << 'Z';

    return rfc3339.str();
}

/// @brief convert embedded timestamp to nanoseconds since POSIX epoch
auto Log::Base::time_epoch_ns() const -> uint64_t
{
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(
        time_point).time_since_epoch().count();
}

void Log::Misc::serialize(std::ostream& log_stream, std::ostream& term_stream) const
{
    term_stream << time_rfc3339() << ' ' << msg << std::endl;

    Json::Object{{
        { "_time", std::move(time_rfc3339()) },
        { "_time_ns", (int64_t) time_epoch_ns() },
        { "_type", Json::Null{} },
        { "message", msg }
    }}.dump(&log_stream);
    log_stream << '\n';
}
