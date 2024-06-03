#ifndef MPSC_HPP
#define MPSC_HPP

#include <atomic>
#include <cstdint>
#include <optional>

#ifdef TEST_MPSC
    #include "../test/test.hpp"
#endif

template <typename T, std::size_t N, std::size_t A = 64>
class mpsc {
    // N must be power of two
    static_assert((N & (N - 1)) == 0,
                  "requested N is not a power of two");

public:
    constexpr auto size() -> std::size_t
        { return N; }

    auto push(T&& obj) -> std::size_t
    {
        // try to allocate slot
        std::size_t _used = used.fetch_add(1, std::memory_order_acquire);
        if (_used >= N) {
            used.fetch_sub(1, std::memory_order_release);
            lost.fetch_add(1, std::memory_order_relaxed);
            return N;
        }

        // allocation succeeded, so increment head
        std::size_t _head = head.fetch_add(1, std::memory_order_acquire)
                          & (N - 1);

        // move data into slot and validate it
        ring[_head].data = std::move(obj);
#ifdef TEST_MPSC
        // slot must be have been invalid while writing
        if (ring[_head].valid.exchange(true, std::memory_order_release))
            throw test_error{"producer got valid slot"};
#else
        ring[_head].valid.store(true, std::memory_order_release);
#endif

        return _used;
    }

    auto pop() -> std::optional<T>
    {
        // try to get used slot
        std::size_t _used = used.load(std::memory_order_relaxed);
#ifdef TEST_MPSC
        // use count may overflow, but must never underflow
        if (_used > SIZE_MAX / 2)
            throw test_error{"used count underflowed"};
#endif
        if (_used == 0)
            return std::nullopt;

        // verify data is valid
        std::size_t _tail = tail & (N - 1);
        if (!ring[_tail].valid.load(std::memory_order_acquire))
            return std::nullopt;

        // move data into optional via std::optional<T>::emplace
        std::optional<T> obj;
        obj.emplace(std::move(ring[_tail].data));

        // invalidate data and release slot
        ring[_tail].valid.store(false, std::memory_order_release);
        used.fetch_sub(1, std::memory_order_release);
        tail++;

        return obj;
    }

private:
    // place shared members on alignment boundary (separate cache line)
    alignas(A) std::atomic<std::size_t> used {0};

    // place producer members alignment boundary (separate cache line)
    alignas(A) std::atomic<std::size_t> head {0};
               std::atomic<std::size_t> lost {0};

    // place consumer members on alignment boundary (separate cache line)
    alignas(A) std::size_t tail {0};

    // place ring on alignment boundary (separate cache line)
    alignas(A) struct {
        std::atomic<bool> valid;
        T data;
    } ring[N] {};
};

#endif /* MPSCP_HPP */
