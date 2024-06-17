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

class config_error : public error {
public:
    config_error(const std::string& message,
                 const int lineno,
                 const std::string& line,
                 const std::source_location location
                     = std::source_location::current())
    {
        std::ostringstream buf;
        buf << "config_error: "
            << location.file_name() << ':' << location.line()
            << ": error parsing config line " << lineno << ": "
            << message << ": " << line;
        what_str = std::move(buf.str());
    }
};

class generic_error : public error {
public:
    generic_error(const std::string& message,
                  const std::source_location location
                      = std::source_location::current())
    {
        std::stringstream buf;
        buf << "generic_error: "
            << location.file_name() << ':' << location.line() << ": "
            << message;
        what_str = std::move(buf.str());
    }
};

class not_implemented_error : public error {
public:
    not_implemented_error(const std::string& message,
                          const std::source_location location
                              = std::source_location::current())
    {
        std::stringstream buf;
        buf << "not_implemented_error: "
            << location.file_name() << ':' << location.line() << ": "
            << message << ": " << message;
        what_str = std::move(buf.str());
    }
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
