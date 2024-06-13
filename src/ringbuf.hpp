#ifndef RINGBUF_HPP
#define RINGBUF_HPP

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
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

template<typename T>
class Ringbuf {
public:
    using sptr = std::shared_ptr<Ringbuf>;

    Ringbuf(const std::filesystem::path& desc_path, size_t desc_size,
            const std::filesystem::path& ring_path, size_t ring_size)
        : shm_desc(desc_path, desc_size)
        , shm_ring(ring_path, ring_size)
    {
        desc = (struct ringbuf *) shm_desc.addr;

        _size = ring_size / sizeof(T);
        _mask = _size - 1;

        // zero-initialize descriptor
        desc->start_nsec = 0;
        desc->sample_rate_hz = 0.;
        desc->head = 0;
        desc->clob = 0;
    }

    // delete copy constructor and copy assignment operator
    Ringbuf(const Ringbuf&) = delete;
    Ringbuf& operator=(const Ringbuf&) = delete;

    auto get_head(std::memory_order m = std::memory_order_relaxed) const -> uint64_t
    { return desc->head.load(m); }

    auto get_start_nsec() const -> uint64_t
    { return desc->start_nsec; }

    auto get_sample_rate_hz() const -> double
    { return desc->sample_rate_hz; }

    auto get_producer_span(size_t size) -> std::span<volatile T>
    {
        assert(desc->clob >= desc->head);

        // requested span does not extend past end of buffer (no wrapping)
        if ((_head & _mask) + size <= _size) {
            T *data = (T *) shm_ring.addr + (_head & _mask);
            desc->clob.store(_head + size, std::memory_order_release);
            return {data, size};
        } else {
            size = _size - (_head & _mask);
            assert((_head & _mask) + size <= _size);
            T *data = (T *) shm_ring.addr + (_head & _mask);
            desc->clob.store(_head + size, std::memory_order_release);
            return {data, size};
        }
    };

    void produce(size_t size)
    {
        assert(_head <= desc->clob);

        _head += size;
        desc->head.store(_head, std::memory_order_release);
    }

    void set_descriptor_info(uint64_t start_nsec, double sample_rate_hz)
    {
        desc->start_nsec = start_nsec;
        desc->sample_rate_hz = sample_rate_hz;
    };

#ifndef TEST_RINGBUF
private:
#endif
    ShmMmap shm_desc;
    ShmMmap shm_ring;

    struct ringbuf *desc;

    // local non-atomic descriptor values to minimize overhead for producer
    uint64_t _head {0};
    uint64_t _size {0};
    uint64_t _mask {0};
};

#endif /* RINGBUF_HPP */
