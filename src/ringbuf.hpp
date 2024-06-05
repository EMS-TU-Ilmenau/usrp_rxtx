#ifndef RINGBUF_HPP
#define RINGBUF_HPP

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "ringbuf.h"

class ShmMmap {
public:
    ShmMmap(const std::filesystem::path& path, size_t size);
    ~ShmMmap();

    // delete copy constructor and copy assignment operator
    ShmMmap(const ShmMmap&) = delete;
    ShmMmap& operator=(const ShmMmap&) = delete;

    std::filesystem::path path;
    void *addr;
    size_t size;

private:
    int fd;
};

class Ringbuf {
public:
    using sptr = std::shared_ptr<Ringbuf>;

    Ringbuf(const std::filesystem::path& desc_path, size_t desc_size,
            const std::filesystem::path& ring_path, size_t ring_size);

    // delete copy constructor and copy assignment operator
    Ringbuf(const Ringbuf&) = delete;
    Ringbuf& operator=(const Ringbuf&) = delete;

private:
    ShmMmap shm_desc;
    ShmMmap shm_ring;

    struct ringbuf *desc;
};

#endif /* RINGBUF_HPP */
