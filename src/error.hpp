#ifndef ERROR_HPP
#define ERROR_HPP

#include <cerrno>
#include <cstring>
#include <exception>
#include <source_location>
#include <sstream>
#include <string>

class error : public std::exception {
public:
    virtual auto
    what() const noexcept -> const char*
    {
        return what_str.c_str();
    }

protected:
    std::string what_str;
};

class syscall_error : public error {
public:
    syscall_error(const std::string& message,
                  const int error_code=errno,
                  const std::source_location location
                      = std::source_location::current())
    {
        std::stringstream buf;
        buf << "syscall_error: "
            << location.file_name() << ':' << location.line() << ": "
            << message << ": " << std::strerror(error_code)
            << " (" << error_code << ')';
        what_str = std::move(buf.str());
    }
};

#endif /* ERROR_HPP */
