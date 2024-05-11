#ifndef MPSC_HPP
#define MPSC_HPP

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>

template <typename T, std::size_t capacity,
          std::size_t alignment = std::hardware_destructive_interference_size>
class mpsc {
    // capacity must be power of two
    static_assert((capacity & (capacity - 1)) == 0,
                  "requested capacity is not a power of two");

public:
    auto push(T &&obj) -> bool;
    auto pop(void) -> std::optional<T>;

private:
    // place shared members on alignment boundary (separate cache line)
    alignas(alignment) std::atomic<std::size_t> used = 0;

    // place producer members alignment boundary (separate cache line)
    alignas(alignment) std::atomic<std::size_t> head = 0;
                       std::atomic<uint_fast32_t> lost = 0;

    // place consumer members on alignment boundary (separate cache line)
    alignas(alignment) std::size_t tail = 0;

    // place ring on alignment boundary (separate cache line)
    alignas(alignment) struct {
        std::atomic<bool> valid;
        T data;
    } ring[capacity] = {};
};

template <typename T, std::size_t capacity, std::size_t alignment>
auto mpsc<T, capacity, alignment>::push(T&& obj) -> bool
{
    // try to allocate slot
    std::size_t _used = used.fetch_add(1, std::memory_order_acquire);
    if (_used >= capacity) {
        used.fetch_sub(1, std::memory_order_release);
        lost.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // allocation succeded, so increment head
    std::size_t _head = head.fetch_add(1, std::memory_order_acquire)
                      & (capacity - 1);

    // move data into slot and validate it
    ring[_head].data = std::move(obj);
#ifdef TEST_MPSC
    // slot must have been invalid while writing
    if (ring[_head].valid.exchange(true, std::memory_order_release))
        throw std::logic_error("unit test failed: used count underflowed");
#else
    ring[_head].valid.store(true, std::memory_order_release);
#endif

    return true;
}

template <typename T, std::size_t capacity, std::size_t alignment>
auto mpsc<T, capacity, alignment>::pop(void) -> std::optional<T>
{
    // try to get used slot
    std::size_t _used = used.load(std::memory_order_relaxed);
#ifdef TEST_MPSC
    // use count may overflow, but must never underflow
    if (_used > SIZE_MAX / 2)
        throw std::logic_error("unit test failed: used count underflowed");
#endif
    if (_used == 0)
        return std::nullopt;

    // verify data is valid
    size_t _tail = tail & (capacity - 1);
    if (!ring[_tail].valid.load(std::memory_order_acquire))
        return std::nullopt;

    // move data into optional via std::optional<T>::emplace
    std::optional<T> obj;
    obj.emplace(ring[_tail].data);

    // invalidate data and release slot
    ring[_tail].valid.store(false, std::memory_order_release);
    used.fetch_sub(1, std::memory_order_release);
    tail++;

    return obj;
}

#endif /* MPSCP_HPP */
