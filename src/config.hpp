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
        std::string mount_desc;
        std::string mount_ring;
        std::size_t size_desc_mib;
        std::size_t size_ring_mib;
    } shmem;
    struct {
        std::string subdev;
        double rate;
        double freq_rf;
        double freq_dsp;
        double gain;
    } rx;
    struct {
        std::string subdev;
        std::string file;
        double rate;
        double freq_rf;
        double freq_dsp;
        double gain;
    } tx;

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
        { "shmem", {
            { "mount_desc", "/dev/hugepages" },
            { "mount_ring", "/dev/hugepages" },
            { "size_desc_mib", 2 },
            { "size_ring_mib", 1024 }
        }},
        { "rx", {
            { "subdev", "" },
            { "rate", 0. },
            { "freq_rf", 0. },
            { "freq_dsp", 0. },
            { "gain", 0. }
        }},
        { "tx", {
            { "subdev", "" },
            { "file", "" },
            { "rate", 0. },
            { "freq_rf", 0. },
            { "freq_dsp", 0. },
            { "gain", 0. }
        }}
    };

    /// valid characters for section and key names
    const std::string valid_chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
};

#endif /* CONFIG_HPP */
