extern "C" {
#ifdef _GNU_SOURCE
    #include <pthread.h>
#endif
#ifdef _POSIX_C_SOURCE
    #include <poll.h>
    #include <sys/socket.h>
#endif
}

#include "mqtt.hpp"

// TODO: implement configuration option for SNDBUF size
static const size_t sndbuf = 32768;

MqttClient::MqttClient(Logger::sptr logger, const Config& cfg)
    : logger{logger}
{
    // append hostname to MQTT topic prefix
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        throw syscall_error{"gethostname() failed"};
    prefix = cfg.mqtt.prefix + "/" + hostname + "/" + cfg.usrp.args;

    // FIXME: assumes MqttClient() is used as singleton
    mosquitto_lib_init();

    int rc = 0;
    mosq = mosquitto_new(nullptr, true, this);
    if (mosq == NULL)
        throw mqtt_error{"mosquitto_new() failed", rc};

    // set TCP_NODELAY to send latency-sensitive messages immediately
    rc = mosquitto_int_option(mosq, MOSQ_OPT_TCP_NODELAY, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        throw mqtt_error{"mosquitto_int_option() failed", rc};
    }

    rc = mosquitto_username_pw_set(mosq,
        cfg.mqtt.user.empty() ? nullptr : cfg.mqtt.user.c_str(),
        cfg.mqtt.password.empty() ? nullptr : cfg.mqtt.password.c_str()
    );
    if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        throw mqtt_error{"mosquitto_username_pw_set() failed", rc};
    }

    rc = mosquitto_threaded_set(mosq, true);
    if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        throw mqtt_error{"mosquitto_threaded_set() failed", rc};
    }

    rc = mosquitto_connect(mosq, cfg.mqtt.host.c_str(), cfg.mqtt.port, 5);
    if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        throw mqtt_error{"mosquitto_connect() failed", rc};
    }

#if _POSIX_C_SOURCE >= 200809L
    int sockfd = mosquitto_socket(mosq);
    if (sockfd == -1) {
        mosquitto_destroy(mosq);
        throw generic_error{"mosquitto_socket() failed"};
    }

    rc = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (rc != 0) {
        mosquitto_destroy(mosq);
        throw syscall_error{"setsockopt() failed"};
    }
#endif

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

void MqttClient::publish(const std::string& topic, const std::string& payload)
{
    // silently discard message if socket send buffer is full to avoid backlog
    // of unsent messages, as libmosquitto will internally buffer messages as
    // long as the socket is connected.
#if _POSIX_C_SOURCE >= 200809L
    struct pollfd pfd { mosquitto_socket(mosq), POLLOUT, 0 };
    int ret = poll(&pfd, 1, 0);
    if (ret == -1) {
        logger->log_exception(std::make_exception_ptr(
            syscall_error{"poll() failed"}
        ));
    }
    if (ret < 1 || (pfd.revents & POLLOUT) == 0)
        return;
#endif

    std::string prefixed_topic = topic.empty() ? prefix : prefix + "/" + topic;
    // TODO: expiry
    // https://mosquitto.org/api/files/mosquitto-h.html#mosquitto_property_add_int32
    //mosquitto_property *proplist = NULL;
    //mosquitto_property_add_int32(&proplist, MQTT_PROP_MESSAGE_EXPIRY_INTERVAL, 86400);
    int rc = mosquitto_publish(mosq, nullptr, prefixed_topic.c_str(), payload.size(), payload.c_str(), 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        logger->log_exception(std::make_exception_ptr(
            mqtt_error{"mosquitto_publish() failed", rc}
        ));
    }
}

void MqttClient::publish(const std::string& topic, std::span<const std::byte> payload)
{
    // silently discard message if socket send buffer is full to avoid backlog
    // of unsent messages, as libmosquitto will internally buffer messages as
    // long as the socket is connected.
#if _POSIX_C_SOURCE >= 200809L
    struct pollfd pfd { mosquitto_socket(mosq), POLLOUT, 0 };
    int ret = poll(&pfd, 1, 0);
    if (ret == -1) {
        logger->log_exception(std::make_exception_ptr(
            syscall_error{"poll() failed"}
        ));
    }
    if (ret < 1 || (pfd.revents & POLLOUT) == 0)
        return;
#endif

    std::string prefixed_topic = topic.empty() ? prefix : prefix + "/" + topic;
    // TODO: expiry
    // https://mosquitto.org/api/files/mosquitto-h.html#mosquitto_property_add_int32
    //mosquitto_property *proplist = NULL;
    //mosquitto_property_add_int32(&proplist, MQTT_PROP_MESSAGE_EXPIRY_INTERVAL, 86400);
    int rc = mosquitto_publish(mosq, nullptr, prefixed_topic.c_str(), payload.size_bytes(), payload.data(), 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        logger->log_exception(std::make_exception_ptr(
            mqtt_error{"mosquitto_publish() failed", rc}
        ));
    }
}

void MqttClient::worker()
try {
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "mqtt");
#endif

    int rc;
    while (run.load(std::memory_order_relaxed)) {
        // run mosquitto network loop (will return early and often on activity)
        rc = mosquitto_loop(mosq, 100, 1);
        if (rc == MOSQ_ERR_SUCCESS)
            continue;

        logger->log_exception(std::make_exception_ptr(
            mqtt_error{"mosquitto_loop() failed", rc}
        ));

        // FIXME: blindly reconnecting regardless of actual error
        // FIXME: mosquitto_reconnect() invokes connect() syscall, which may
        //        block for several minutes, stalling reconnection attempts
        //        even when the network is up again. need to hack connect
        //        timeouts into libmosquitto:
        //        https://stackoverflow.com/questions/2597608/c-socket-connection-timeout
        rc = mosquitto_reconnect(mosq);
        if (rc == MOSQ_ERR_SUCCESS) {
#if _POSIX_C_SOURCE >= 200809L
            int sockfd = mosquitto_socket(mosq);
            if (sockfd == -1)
                continue;
            rc = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
            if (rc != 0) {
                logger->log_exception(std::make_exception_ptr(
                    syscall_error{"setsockopt() failed"}
                ));
            }
#endif
            continue;
        }

        logger->log_exception(std::make_exception_ptr(
            mqtt_error{"mosquitto_reconnect() failed", rc}
        ));
        std::this_thread::sleep_for(std::chrono::seconds(5));
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
