#ifndef MQTT_HPP
#define MQTT_HPP

#include <cerrno>
#include <cstring>

#include <chrono>
#include <exception>
#include <iostream>
#include <source_location>
#include <span>
#include <sstream>
#include <string>
#include <thread>

#include "config.hpp"
#include "error.hpp"
#include "logging.hpp"
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

class MqttClient {
public:
    using sptr = std::shared_ptr<MqttClient>;

    MqttClient(Logger::sptr logger, const Config& cfg);
    ~MqttClient();

    // delete copy constructor and copy assignment operator
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    auto is_running() -> bool
    { return run.load(std::memory_order_relaxed); }

    void publish(const std::string& topic, const std::string& payload);
    void publish(const std::string& topic, std::span<const std::byte> payload);

private:
    std::atomic<bool> run {true};
    std::thread worker_handle;

    Logger::sptr logger;
    struct mosquitto *mosq;

    std::string prefix;

    void worker();
};

#endif /* MQTT_HPP */
