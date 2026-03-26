// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <system_error>

extern "C" {
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <unistd.h>
}

#include "error.hpp"
#include "ringbuf.hpp"

ShmMmap::ShmMmap(const std::filesystem::path& path, size_t size)
{
    this->size = size;
    this->path = path;

    // create file
    fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd == -1)
        throw syscall_error{"open(\"" + path.generic_string() + "\", ...) failed"};

    // grow file to requested size
    if (ftruncate(fd, size) == -1) {
        close(fd);
        unlink(path.c_str());

        throw syscall_error{"ftruncate() failed"};
    }

    // create memory mapping
    addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
        close(fd);
        unlink(path.c_str());

        throw syscall_error{"mmap() failed"};
    }

    // kernel employs lazy allocation, which will result in SIGSEGV if
    // allocation fails. zero memory now to enforce full allocation.
    std::memset(addr, 0x00, size);
}

ShmMmap::~ShmMmap()
{
    munmap(addr, size);
    close(fd);
    unlink(path.c_str());
}
