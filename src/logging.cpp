#include "error.hpp"
#include "logging.hpp"
#include "version.hpp"

Logger::Logger(const std::filesystem::path& path_prefix)
{
    uint64_t epoch_sec = std::chrono::time_point_cast<std::chrono::seconds>(
            std::chrono::system_clock::now()).time_since_epoch().count();

    // open log file
    std::filesystem::path path = path_prefix;
    path += "_" + std::to_string(epoch_sec) + ".log";
    log_file.open(path);
    if (!log_file)
        throw syscall_error{"error opening log file: " + path.generic_string()};

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
            const auto [str_log, str_term] = std::visit([](auto& arg){ return arg.serialize(); }, opt.value());
            if (str_term.has_value())
                std::cout << str_term.value() << std::endl;
            log_file << str_log << '\n';
        }
    }
}

auto Log::Base::serialize() -> std::tuple<std::string, std::optional<std::string>>
{
    return {"", std::nullopt};
}

auto Log::Misc::serialize() -> std::tuple<std::string, std::optional<std::string>>
{
    return {msg, msg};
}
