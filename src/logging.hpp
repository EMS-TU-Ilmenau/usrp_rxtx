#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <thread>
#include <variant>

#include "mpsc.hpp"

class Logging {
public:
    using sptr = std::shared_ptr<Logging>;

    static sptr make(const std::filesystem::path& path)
        { return sptr(new Logging(path)); }
    ~Logging(void);

    void log(std::string&& mesg)
        { log_queue.push(std::move(mesg)); }

private:
    Logging(const std::filesystem::path& path_prefix);

    std::atomic<bool> run = false;
    std::thread worker_handle;
    mpsc<std::string, 128> log_queue;

    std::ofstream log_file;

    void worker(void);
};

#endif /* LOGGING_HPP */
