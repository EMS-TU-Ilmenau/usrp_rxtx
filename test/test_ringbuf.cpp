#define TEST_RINGBUF

#include "test.hpp"
#include "../src/ringbuf.hpp"

#include <cstring>
#include <chrono>
#include <complex>
#include <filesystem>
#include <iostream>
#include <random>

extern "C" {
    #include <unistd.h>
}

using sample_t = std::complex<int16_t>;

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    uint64_t epoch_nsec = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();

    std::filesystem::path desc_path {"/tmp"};
    std::filesystem::path ring_path {"/tmp"};
    std::string file_prefix {"usrp_rxtx_test_ringbuf_"
                            + std::to_string(getpid()) + "_"
                            + std::to_string(epoch_nsec)};
    desc_path /= file_prefix + "_desc";
    ring_path /= file_prefix + "_ring";

    // own head and clob pointers to test Ringbuf against
    uint64_t head = 0, clob = 0;
    size_t ring_size = 4096;
    size_t ring_mask = ring_size - 1;

    // allocate Ringbuf and extract data pointer
    Ringbuf<sample_t> ring {desc_path, 4096, ring_path, ring_size * sizeof(sample_t)};
    volatile sample_t *ring_data = (volatile sample_t *) ring.shm_ring.addr;

    // fuzz Ringbuf with random production
    std::default_random_engine rand_eng {3'141'592'653UL};
    std::uniform_int_distribution<size_t> uniform_dist {0, 1024};
    for (size_t n = 0; n < 1'000'000UL; n++) {
        // get span of random size
        size_t span_size = uniform_dist(rand_eng);
        auto span = ring.get_producer_span(span_size);

        // returned span may must be as long as requested or end with the ring
        if (span.size() != span_size && span.size() != (ring_size - (head & ring_mask)))
            throw test_error{"span size invalid"};

        // check clob pointer
        clob = head + span.size();
        if (ring.desc->clob != clob)
            throw test_error{"clob pointer invalid"};

        // check data address
        if (span.data() != &ring_data[head & ring_mask])
            throw test_error{"span data address invalid"};

        // write into span
        std::memset((void *) span.data(), n & 0xff, span.size_bytes());

        // produce random amount up to size of span
        size_t prod_size = std::min(span.size(), uniform_dist(rand_eng));
        ring.produce(prod_size);

        // check head
        head += prod_size;
        if (ring.desc->head != head)
            throw test_error{"head pointer invalid"};
    }

    return 0;
}
