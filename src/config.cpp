// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#include <charconv>
#include <fstream>

#include "config.hpp"
#include "error.hpp"

// helper template for overloaded lambdas as visitors for std::visit
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

// conversion from std::variant to another std::variant with superset of types
// https://stackoverflow.com/questions/47203255/convert-stdvariant-to-another-stdvariant-with-super-set-of-types/47204507#47204507
template <class... Args>
struct variant_cast_proxy
{
    std::variant<Args...> v;

    template <class... ToArgs>
    operator std::variant<ToArgs...>() const
    {
        return std::visit([](auto&& arg) -> std::variant<ToArgs...> { return arg ; }, v);
    }
};
template <class... Args>
auto variant_cast(const std::variant<Args...> &v) -> variant_cast_proxy<Args...>
{
    return {v};
}

Config::Config(std::istream& istream)
{
    parse(istream);
}

Config::Config(const std::filesystem::path& path)
{
    std::ifstream ifstream{path};
    if (!ifstream)
        throw syscall_error{"error opening config file " + path.generic_string()};
    parse(ifstream);
    ifstream.close();
}

void Config::parse(std::istream& istream)
{
    int lineno = 0;
    std::string section{""};
    for (std::string line; std::getline(istream, line); ) {
        if (!istream)
            throw syscall_error{"error reading config"};

        lineno++;
        std::string_view view{line};

        // skip empty lines
        if (view.empty())
            continue;

        // skip comments
        if (view.starts_with('#') || view.starts_with(';'))
            continue;

        // trim trailing whitespace
        std::size_t end = view.find_last_not_of("\t\r ");
        if (end == std::string::npos)
            continue;
        view = view.substr(0, end + 1);

        // parse section name
        if (view.starts_with('[') && view.ends_with(']')) {
            section = view.substr(1, view.length() - 2);

            // validate section name
            if (section.find_first_not_of(valid_chars) != std::string::npos)
                throw config_error{"invalid section name", lineno, line};

            // verify section name exists in config
            if (map.find(section) == map.end())
                throw config_error{"unknown section name", lineno, line};

            continue;
        }

        // whitespace, comments, and section names were processed above, so
        // only key value pairs remain

        // parse key: key ends at first invalid character (e.g., space or =)
        std::size_t key_len = view.find_first_not_of(valid_chars);
        std::string key{view.substr(0, key_len)};

        // first non-whitespace character after key must be equal sign
        std::size_t equal_off = view.find_first_not_of(" \t", key_len);
        if (view[equal_off] != '=')
            throw config_error{"not a key value pair", lineno, line};

        // verify key name exists in config
        if (map[section].find(key) == map[section].end())
            throw config_error{"unknown key name", lineno, line};

        // first non-whitespace character after equal sign is start of value
        std::size_t val_off = view.find_first_not_of(" \t", equal_off + 1);
        view = val_off < view.length() ? view.substr(val_off) : "";

        // overloaded lambda visitor that parses string_view of config value
        auto visitor = overloaded {
        [view](std::string& val) {
            val = view;
        },
        [view, lineno, line](double& val) {
            auto res = std::from_chars(view.begin(), view.end(), val);
            if (res.ec != std::errc() || res.ptr != view.end())
                throw config_error{"value type not float", lineno, line};
        },
        [view, lineno, line](int64_t& val) {
            auto res = std::from_chars(view.begin(), view.end(), val);
            if (res.ec != std::errc() || res.ptr != view.end())
                throw config_error{"value type not integer", lineno, line};
        }};

        std::visit(visitor, map[section][key]);
    }

    // copy variant values to static type structs
    usrp.args = std::get<std::string>(map["usrp"]["args"]);
    usrp.sync = std::get<std::string>(map["usrp"]["sync"]);
    cpu.max_latency_usec = (int32_t) std::get<int64_t>(map["cpu"]["max_latency_usec"]);
    mqtt.host = std::get<std::string>(map["mqtt"]["host"]);
    mqtt.port = (int) std::get<int64_t>(map["mqtt"]["port"]);
    mqtt.user = std::get<std::string>(map["mqtt"]["user"]);
    mqtt.password = std::get<std::string>(map["mqtt"]["password"]);
    mqtt.prefix = std::get<std::string>(map["mqtt"]["prefix"]);
    mqtt.pub_samples = (size_t) std::get<int64_t>(map["mqtt"]["pub_samples"]);
    shmem.mount_desc = std::get<std::string>(map["shmem"]["mount_desc"]);
    shmem.mount_ring = std::get<std::string>(map["shmem"]["mount_ring"]);
    shmem.size_desc_mib = (std::size_t) std::get<int64_t>(map["shmem"]["size_desc_mib"]);
    shmem.size_ring_mib = (std::size_t) std::get<int64_t>(map["shmem"]["size_ring_mib"]);
    rx.subdev = std::get<std::string>(map["rx"]["subdev"]);
    rx.antenna = std::get<std::string>(map["rx"]["antenna"]);
    rx.bandwidth = std::get<double>(map["rx"]["bandwidth"]);
    rx.freq_rf = std::get<double>(map["rx"]["freq_rf"]);
    rx.freq_dsp = std::get<double>(map["rx"]["freq_dsp"]);
    rx.gain = std::get<double>(map["rx"]["gain"]);
    rx.lo_source = std::get<std::string>(map["rx"]["lo_source"]);
    rx.rate = std::get<double>(map["rx"]["rate"]);
    tx.file = std::get<std::string>(map["tx"]["file"]);
    tx.subdev = std::get<std::string>(map["tx"]["subdev"]);
    tx.antenna = std::get<std::string>(map["tx"]["antenna"]);
    tx.bandwidth = std::get<double>(map["tx"]["bandwidth"]);
    tx.freq_rf = std::get<double>(map["tx"]["freq_rf"]);
    tx.freq_dsp = std::get<double>(map["tx"]["freq_dsp"]);
    tx.gain = std::get<double>(map["tx"]["gain"]);
    tx.lo_source = std::get<std::string>(map["tx"]["lo_source"]);
    tx.rate = std::get<double>(map["tx"]["rate"]);
    wr.directory = std::get<std::string>(map["wr"]["directory"]);
}

auto Config::to_json() const -> Json::Object
{
    Json::Object cfg;

    // iterate over config sections
    for (const auto& iter_sec : map) {
        Json::Object section;

        // iterate over key-value pairs of current section
        for (const auto& iter_kv : iter_sec.second)
            section.emplace_back(iter_kv.first, variant_cast(iter_kv.second));

        cfg.emplace_back(iter_sec.first, section);
    }

    return cfg;
}
