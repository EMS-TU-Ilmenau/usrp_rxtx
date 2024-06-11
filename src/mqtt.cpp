extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
}

#include "mqtt.hpp"

MqttClient::MqttClient(Logger::sptr logger)
{
    // FIXME: assumes MqttClient() is used as singleton
    mosquitto_lib_init();

    int rc = 0;
    mosq = mosquitto_new(nullptr, true, this);
    if (mosq == NULL)
        throw mqtt_error{"mosquitto_new() failed", rc};

    rc = mosquitto_threaded_set(mosq, true);
    if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        throw mqtt_error{"mosquitto_threaded_set() failed", rc};
    }

    rc = mosquitto_connect(mosq, "127.0.0.1", 1883, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        throw mqtt_error{"mosquitto_connect() failed", rc};
    }

    this->logger = logger;

    // spawn worker
    worker_handle = std::thread{&MqttClient::worker, this};
}

MqttClient::~MqttClient()
{
    // stop and join worker thread
    run.store(false, std::memory_order_relaxed);
    worker_handle.join();

    // disconnect and clean up
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    // FIXME: assumes MqttClient() is used as singleton
    mosquitto_lib_cleanup();
}

void MqttClient::worker()
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "mqtt");
#endif

    uint64_t next_heartbeat = 0;

    int rc;
    while (run.load(std::memory_order_relaxed)) {
        // send periodic heartbeat
        uint64_t epoch_nsec = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now()).time_since_epoch().count();
        if (epoch_nsec >= next_heartbeat) {
            std::string heartbeat = std::to_string(epoch_nsec);
            rc = mosquitto_publish(mosq, nullptr, "usrp_rxtx/heartbeat", heartbeat.size(), heartbeat.c_str(), 0, false);
            if (rc != MOSQ_ERR_SUCCESS)
                throw mqtt_error{"mosquitto_publish() failed", rc};
            next_heartbeat = epoch_nsec + 1'000'000'000UL;
        }

        // run mosquitto network loop (will return early and often on activity)
        // TODO: error handling on recoverable errors, most importantly
        //       mosquitto_reconnect() on MOSQ_ERR_CONN_LOST
        rc = mosquitto_loop(mosq, 100, 1);
        if (rc != MOSQ_ERR_SUCCESS)
            throw mqtt_error{"mosquitto_loop() failed", rc};
    }
} catch (const std::exception& e) {
    logger->log_exception(std::current_exception());
    logger->log("Unrecoverable exception occurred in MQTT client thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
} catch (...) {
    logger->log("Unknown exception occurred in MQTT client thread. Thread terminated.",
                Log::FATAL);
    run.store(false, std::memory_order_relaxed);
}
