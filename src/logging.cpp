#include "error.hpp"
#include "logging.hpp"
#include "version.hpp"

Logging::Logging(const std::filesystem::path& path_prefix)
{
    uint64_t epoch_sec = std::chrono::time_point_cast<std::chrono::seconds>(
            std::chrono::system_clock::now()).time_since_epoch().count();

    // open log file
    std::filesystem::path path = path_prefix;
    path += "_" + std::to_string(epoch_sec) + ".log";
    log_file.open(path);
    if (!log_file)
        throw syscall_error("error opening log file: " + path.generic_string());

    // spawn and wait for worker
    worker_handle = std::thread(&Logging::worker, this);
    run.wait(false);
}

Logging::~Logging(void)
{
    if (run.exchange(false))
        worker_handle.join();
    log_file.close();
}

void Logging::worker(void)
{
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "logging");
#endif

    // notify constructor
    run.store(true, std::memory_order_seq_cst);
    run.notify_one();

    while (run.load(std::memory_order_relaxed)) {
        auto opt = log_queue.pop();
        if (!opt.has_value()) {
            // TODO: implement low-overhead notification by producer.
            //       std::atomic<>::wait and std::atomic<>::notify_one may
            //       offer lower overhead than std::condition_variable.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // TODO: proper log handling with std::variant
        std::string mesg = std::move(opt.value());
        std::cout << mesg << std::endl;
        log_file << mesg << '\n';
    }
}
