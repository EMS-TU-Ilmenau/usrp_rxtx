#ifndef SERIAL_HPP
#define SERIAL_HPP

#include <filesystem>

extern "C" {
    #include <fcntl.h>
    #include <termios.h>
    #include <unistd.h>
}

#include "error.hpp"

class TTY {
public:
    TTY(const std::filesystem::path &tty_path, int baudrate)
    {
        // open serial line
        tty_fd = open(tty_path.c_str(), O_WRONLY | O_CLOEXEC | O_NOCTTY);
        if (tty_fd == -1)
            throw syscall_error{"open(" + tty_path.generic_string() + ", ...) failed"};

        // configure raw serial communication
        struct termios tty;
        memset(&tty, '\0', sizeof(tty));
        tty.c_cflag = baudrate | CS8 | CLOCAL | CREAD;
        tty.c_iflag = 0;
        tty.c_oflag = 0;
        tty.c_lflag = 0;
        tty.c_cc[VMIN]  = 1;
        tty.c_cc[VTIME] = 0;
        tcflush(tty_fd, TCIFLUSH);
        tcsetattr(tty_fd, TCSANOW, &tty);
    }

    ~TTY()
    {
        close(tty_fd);
    }

    // delete copy constructor and copy assignment operator
    TTY(const TTY&) = delete;
    TTY& operator=(const TTY&) = delete;

    auto wr(const std::string& buf) -> ssize_t
    {
        ssize_t ret = write(tty_fd, buf.c_str(), buf.size());
        if (ret == -1)
            throw syscall_error{"write() failed"};
        tcdrain(tty_fd);
        return ret;
    }

private:
    int tty_fd;
};

#endif /* SERIAL_HPP */
