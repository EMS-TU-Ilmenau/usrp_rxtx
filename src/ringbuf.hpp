// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#ifndef RINGBUF_HPP
#define RINGBUF_HPP

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "error.hpp"
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

    auto get_backlog_samples(uint64_t tail) const -> uint64_t
    {
        uint64_t head = desc->head.load(std::memory_order_relaxed);
        if (tail >= head)
            return 0;
        return head - tail;
    }

    auto get_clobber_distance(uint64_t tail) const -> int64_t
    {
        uint64_t clob = desc->clob.load(std::memory_order_relaxed);
        return tail + _size - clob;
    }

    auto get_consumer_span(uint64_t tail, size_t size) const -> std::span<const volatile T>
    {
        T *data = (T *) shm_ring.addr + (tail & _mask);
        uint64_t head = desc->head.load(std::memory_order_relaxed);

        // return empty span if requested data was not yet fully produced
        if (tail + size > head) [[unlikely]] {
            return {data, 0};
        }

        // return span, handling ring buffer wrapping
        // TODO: throw exception instead of returning already clobbered data?
        if ((tail & _mask) + size <= _size) {
            // no wrapping
            return {data, size};
        } else {
            // wrapping: clip size to end of ring buffer
            size = _size - (tail & _mask);
            assert((tail & _mask) + size <= _size);
            return {data, size};
        }
    }

    auto get_consumer_wait(uint64_t tail, size_t size) const -> uint64_t
    {
        uint64_t head = desc->head.load(std::memory_order_relaxed);
        const double sleep_sec = (tail + size - head) / desc->sample_rate_hz;
        if (sleep_sec <= 0) {
            return 0;
        } else {
            return sleep_sec * 1e9;
        }
    }

    auto get_aligned_samples(size_t size) -> std::span<const T>
    {
        // get producer head aligned to multiple of size
        uint64_t head = desc->head.load(std::memory_order_relaxed);
        head -= head % size;

        // requested amount of samples fits in ring buffer
        if ((head & _mask) >= size) {
            T *data = (T *) shm_ring.addr + (head & _mask) - size;
            return {data, size};
        } else {
            T *data = (T *) shm_ring.addr + ((head - size) & _mask) - size;
            return {data, size};
        }
    }

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

    auto get_tail(uint64_t tail_nsec) -> uint64_t
    {
        // unable to satisfy tail prior to start of buffer
        if (tail_nsec < desc->start_nsec) [[unlikely]]
            throw generic_error{"requested tail precedes start time of buffer"};

        // convert tail from nanoseconds to samples
        const uint64_t tail = std::round(
            (tail_nsec - desc->start_nsec) * 1e-9 * desc->sample_rate_hz
        );

        // verify tail has not been clobbered by producer already
        if (tail <= desc->clob.load(std::memory_order_relaxed)) [[unlikely]]
            throw generic_error{"requested tail already clobbered"};

        return tail;
    }

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
