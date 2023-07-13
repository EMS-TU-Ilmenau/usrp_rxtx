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
        double sample_rate;
    } usrp;
    struct {
        int32_t max_latency_usec;
    } cpu;
    struct {
        std::string mount_desc;
        std::string mount_data;
        std::size_t size_desc_mib;
        std::size_t size_data_mib;
    } shmem;
    struct {
        std::string subdev;
        double gain;
    } rx;
    struct {
        std::string subdev;
        double gain;
    } tx;

    /// convert config to Json::Object for logging config values
    auto to_json() const -> Json::Object;

private:
    void parse(std::istream& istream);

    typedef std::variant<std::string, int64_t, double> value_t;

    /// nested map of above structs containing default values
    std::map<std::string, std::map<std::string, value_t>> map = {
        { "usrp", {
            { "args", "" },
            { "sample_rate", 0. }
        }},
        { "cpu", {
            { "max_latency_usec", 100 }
        }},
        { "shmem", {
            { "mount_desc", "/dev/hugepages" },
            { "mount_data", "/dev/hugepages" },
            { "size_desc_mib", 2 },
            { "size_data_mib", 1024 }
        }},
        { "rx", {
            { "subdev", "" },
            { "gain", 0. }
        }},
        { "tx", {
            { "subdev", "" },
            { "gain", 0. }
        }}
    };

    /// valid characters for section and key names
    const std::string valid_chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
};

#endif /* CONFIG_HPP */
