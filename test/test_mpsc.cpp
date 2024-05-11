#include <atomic>
#include <iostream>
#include <thread>

#include "test.hpp"

#define TEST_MPSC
#include "../src/mpsc.hpp"

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

#ifndef NUM_ITERATIONS
#define NUM_ITERATIONS 1'000'000UL
#endif

struct item {
    uint64_t thread;
    uint64_t ctr;
};

static mpsc<struct item, 128, CACHE_LINE_SIZE> queue;
static volatile std::atomic<bool> run{true};

// producer worker pushing increasing counter values into queue, only
// incrementing counter when value was pushed successfully
void producer(uint64_t thread)
{
    uint64_t ctr = 0;
    while (ctr < NUM_ITERATIONS) {
        struct item s { thread, ctr };
        if (queue.push(std::move(s)))
            ctr++;
    }
}

// consumer worker that pops from queue and verifies that counter values are
// consecutive
void consumer(void)
{
    uint64_t ctr[2] = { 0, 0 };

    while (run) {
        auto obj = queue.pop();
        if (obj.has_value()) {
            struct item s = std::move(obj.value());
            if (ctr[s.thread]++ != s.ctr)
                throw test_error{"invalid counter value"};
        }
    }

    // verify final counter value is reached
    if (ctr[0] != NUM_ITERATIONS || ctr[1] != NUM_ITERATIONS)
        throw test_error{"invalid final counter value"};
}

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    // spawn consumer and two producers
    std::thread c{consumer};
    std::thread p1{producer, 0};
    std::thread p2{producer, 1};

    // gracefully terminate consumer after both producers finish
    p1.join();
    p2.join();
    run = false;
    c.join();

    return 0;
}
