// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#ifndef RINGBUF_H
#define RINGBUF_H

#ifdef __cplusplus
    #include <atomic>
    #include <cstdint>
    using atomic_uint64_t = std::atomic<std::uint64_t>;
#else
    #include <stdalign.h>
    #include <stdatomic.h>
    #include <stdint.h>
#endif

#ifdef __STDC_NO_ATOMICS__
    #error "C11 atomics not supported by compiler"
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct ringbuf {
    /// producer timestamp (nsec) of first sample that went into the buffer
    uint64_t start_nsec;

    /// producer sample rate in Hz
    double sample_rate_hz;

    /// producer head offset in samples
    volatile atomic_uint64_t head;

    /// producer clobber offset in samples
    volatile atomic_uint64_t clob;
};

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */
