#ifndef TEST_HPP
#define TEST_HPP

#include <exception>
#include <source_location>
#include <sstream>
#include <string>

class test_error : public std::exception {
public:
    test_error(const std::string& message,
               const std::source_location location
                   = std::source_location::current())
    {
        std::ostringstream buf;
        buf << location.file_name() << ':' << location.line() << ": "
            << message;
        what_str = std::move(buf.str());
    }

    virtual auto
    what() const noexcept -> const char*
    {
        return what_str.c_str();
    }

protected:
    std::string what_str;
};

#endif /* TEST_HPP */
