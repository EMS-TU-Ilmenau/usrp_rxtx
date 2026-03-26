// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <filesystem>
#include <map>
#include <string>
#include <variant>

#include "json.hpp"

class Config {
public:
    Config(std::istream& istream);
    Config(const std::filesystem::path& path);

    struct {
        std::string args;
        std::string sync;
    } usrp;
    struct {
        int32_t max_latency_usec;
    } cpu;
    struct {
        std::string host;
        int port;
        std::string user;
        std::string password;
        std::string prefix;
        size_t pub_samples;
    } mqtt;
    struct {
        std::string mount_desc;
        std::string mount_ring;
        std::size_t size_desc_mib;
        std::size_t size_ring_mib;
    } shmem;
    struct {
        std::string subdev;
        std::string antenna;
        double bandwidth;
        double freq_rf;
        double freq_dsp;
        double gain;
        std::string lo_source;
        double rate;
    } rx;
    struct {
        std::string file;
        std::string subdev;
        std::string antenna;
        double bandwidth;
        double freq_rf;
        double freq_dsp;
        double gain;
        std::string lo_source;
        double rate;
    } tx;
    struct {
        std::string directory;
    } wr;

    /// convert config to Json::Object for logging config values
    auto to_json() const -> Json::Object;

private:
    void parse(std::istream& istream);

    using value_t = std::variant<std::string, int64_t, double>;

    /// nested map of above structs containing default values
    std::map<std::string, std::map<std::string, value_t>> map = {
        { "usrp", {
            { "args", "" },
            { "sync", "" }
        }},
        { "cpu", {
            { "max_latency_usec", 100 }
        }},
        { "mqtt", {
            { "host", "0.0.0.0" },
            { "port", 1883 },
            { "user", "" },
            { "password", "" },
            { "prefix", "usrp_rxtx" },
            { "pub_samples", 0 }
        }},
        { "shmem", {
            { "mount_desc", "/dev/hugepages" },
            { "mount_ring", "/dev/hugepages" },
            { "size_desc_mib", 2 },
            { "size_ring_mib", 1024 }
        }},
        { "rx", {
            { "subdev", "" },
            { "antenna", "" },
            { "bandwidth", 0. },
            { "freq_rf", 0. },
            { "freq_dsp", 0. },
            { "gain", 0. },
            { "lo_source", "" },
            { "rate", 0. },
        }},
        { "tx", {
            { "file", "" },
            { "subdev", "" },
            { "antenna", "" },
            { "bandwidth", 0. },
            { "freq_rf", 0. },
            { "freq_dsp", 0. },
            { "gain", 0. },
            { "lo_source", "" },
            { "rate", 0. },
        }},
        { "wr", {
            { "directory", "" },
        }}
    };

    /// valid characters for section and key names
    const std::string valid_chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
};

#endif /* CONFIG_HPP */
