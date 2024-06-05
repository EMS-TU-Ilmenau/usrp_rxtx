#ifndef MQTT_HPP
#define MQTT_HPP

#include <cerrno>
#include <cstring>

#include <chrono>
#include <exception>
#include <iostream>
#include <source_location>
#include <sstream>
#include <string>
#include <thread>

#include "error.hpp"
#include "mosquitto.h"

class mqtt_error : public error {
public:
    mqtt_error(const std::string& message,
               const int mqtt_errno,
               const std::source_location location =
                 std::source_location::current())
    {
        std::ostringstream buf;
        buf << location.file_name() << ':' << location.line()
            << ": mqtt_error: " << message << ": "
            << mosquitto_strerror(mqtt_errno) << " (" << mqtt_errno << ')';
        what_str = std::move(buf.str());
    }
};

class Mqtt {
public:
    using sptr = std::shared_ptr<Mqtt>;

    Mqtt();
    ~Mqtt();

    // delete copy constructor and copy assignment operator
    Mqtt(const Mqtt&) = delete;
    Mqtt& operator=(const Mqtt&) = delete;

    void stop();

    inline bool is_running()
        { return run; }
    std::string get_status();

private:
    volatile std::atomic<bool> run;
    std::thread worker_handle;

    struct mosquitto *mosq;

    void worker();
};

#endif /* MQTT_HPP */
