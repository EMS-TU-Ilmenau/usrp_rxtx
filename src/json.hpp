#ifndef JSON_HPP
#define JSON_HPP

#include <charconv>
#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

class Json {
public:
    class Null {};
    class Array;
    class Object;

    typedef std::variant<Null, std::string, uint64_t, int64_t, double, bool, Array, Object> json_t;

    Json() {}
    Json(const Json& other) : variant{other.variant} {}
    Json(const Null& other) : variant{other} {}
    Json(const std::string& other) : variant{other} {}
    Json(const uint64_t& other) : variant{other} {}
    Json(const int64_t& other) : variant{other} {}
    Json(const double& other) : variant{other} {}
    Json(const bool& other) : variant{other} {}
    Json(const Array& other) : variant{other} {}
    Json(const Object& other) : variant{other} {}
    Json(Json&& other) : variant(std::move(other.variant)) {}
    Json(Null&& other) : variant(std::move(other)) {}
    Json(Array&& other) : variant(std::move(other)) {}
    Json(Object&& other) : variant(std::move(other)) {}

    void operator=(const Json& other) { variant = other.variant; }
    void operator=(const Null& other) { variant = other; }
    void operator=(const std::string& other) { variant = other; }
    void operator=(const uint64_t& other) { variant = other; }
    void operator=(const int64_t& other) { variant = other; }
    void operator=(const double& other) { variant = other; }
    void operator=(const bool& other) { variant = other; }
    void operator=(const Array& other) { variant = other; }
    void operator=(const Object& other) { variant = other; }
    void operator=(Json&& other) { variant = std::move(other.variant); }
    void operator=(Null&& other) { variant = std::move(other); }
    void operator=(Array&& other) { variant = std::move(other); }
    void operator=(Object&& other) { variant = std::move(other); }

    inline auto operator*() -> json_t&
    {
        return variant;
    }

    inline auto operator->() -> json_t*
    {
        return &variant;
    }

    inline void dump(std::ostream* stream) const
    {
        std::visit(Visitor{stream}, variant);
    }

    inline auto dumps() const -> std::string
    {
        std::ostringstream stream;
        std::visit(Visitor{&stream}, variant);
        return stream.str();
    }

    class Array : public std::vector<json_t> {
    public:
        inline void dump(std::ostream* stream) const
        {
            Visitor{stream}.operator()(*this);
        }

        inline auto dumps() const -> std::string
        {
            std::ostringstream stream;
            Visitor{&stream}.operator()(*this);
            return stream.str();
        }
    };

    class Object : public std::vector<std::pair<std::string, json_t>> {
    public:
        inline void dump(std::ostream *stream) const
        {
            Visitor{stream}.operator()(*this);
        }

        inline auto dumps() const -> std::string
        {
            std::ostringstream stream;
            Visitor{&stream}.operator()(*this);
            return stream.str();
        }
    };

private:
    json_t variant;

    class Visitor {
    public:
        Visitor(std::ostream* stream)
            : stream{stream}
            , old_precision{stream->precision(15)} {}
        ~Visitor() { stream->precision(old_precision); }

        void operator()(const Null& val) const
        {
            (void) val;
            *stream << "null";
        }

        void operator()(const std::string& val) const
        {
            escape_string(stream, val);
        }

        void operator()(const uint64_t& val) const
        {
            *stream << val;
        }

        void operator()(const int64_t& val) const
        {
            *stream << val;
        }

        void operator()(const double& val) const
        {
            *stream << val;
        }

        void operator()(const bool& val) const
        {
            *stream << (val ? "true" : "false");
        }

        void operator()(const Array& val) const
        {
            *stream << '[';

            // first element
            auto iter = val.begin();
            if (iter != val.end()) {
                std::visit(*this, *iter);
                iter++;
            }

            // consecutive elements
            for (auto end = val.end(); iter != end; iter++) {
                *stream << ", ";
                std::visit(*this, *iter);
            }

            *stream << ']';
        }

        void operator()(const Object& val) const
        {
            *stream << '{';

            // first element
            auto iter = val.begin();
            if (iter != val.end()) {
                escape_string(stream, iter->first);
                *stream << ": ";
                std::visit(*this, iter->second);
                iter++;
            }

            // consecutive elements
            for (auto end = val.end(); iter != end; iter++) {
                *stream << ", ";
                escape_string(stream, iter->first);
                *stream << ": ";
                std::visit(*this, iter->second);
            }

            *stream << '}';
        }

    private:
        std::ostream* stream;
        std::streamsize old_precision;
    };

    static void escape_string(std::ostream* stream, const std::string& str)
    {
        *stream << '"';

        for (const auto& c : str) {
            // https://datatracker.ietf.org/doc/html/rfc8259#section-7
            switch (c) {
            case '\x00': *stream << "\\u0000"; break;
            case '\x01': *stream << "\\u0001"; break;
            case '\x02': *stream << "\\u0002"; break;
            case '\x03': *stream << "\\u0003"; break;
            case '\x04': *stream << "\\u0004"; break;
            case '\x05': *stream << "\\u0005"; break;
            case '\x06': *stream << "\\u0006"; break;
            case '\x07': *stream << "\\u0007"; break;
            case '\x08': *stream << "\\b";     break;
            case '\x09': *stream << "\\t";     break;
            case '\x0a': *stream << "\\n";     break;
            case '\x0b': *stream << "\\u000b"; break;
            case '\x0c': *stream << "\\f";     break;
            case '\x0d': *stream << "\\r";     break;
            case '\x0e': *stream << "\\u000e"; break;
            case '\x0f': *stream << "\\u000f"; break;
            case '\x10': *stream << "\\u0010"; break;
            case '\x11': *stream << "\\u0011"; break;
            case '\x12': *stream << "\\u0012"; break;
            case '\x13': *stream << "\\u0013"; break;
            case '\x14': *stream << "\\u0014"; break;
            case '\x15': *stream << "\\u0015"; break;
            case '\x16': *stream << "\\u0016"; break;
            case '\x17': *stream << "\\u0017"; break;
            case '\x18': *stream << "\\u0018"; break;
            case '\x19': *stream << "\\u0019"; break;
            case '\x1a': *stream << "\\u001a"; break;
            case '\x1b': *stream << "\\u001b"; break;
            case '\x1c': *stream << "\\u001c"; break;
            case '\x1d': *stream << "\\u001d"; break;
            case '\x1e': *stream << "\\u001e"; break;
            case '\x1f': *stream << "\\u001f"; break;
            case '\x22': *stream << "\\\"";    break;
            case '\x5c': *stream << "\\\\";    break;
            default:     *stream << c;
            }
        }

        *stream << '"';
    }
};

#endif /* JSON_HPP */
