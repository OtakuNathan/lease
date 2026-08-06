// pool_stress_tests.cpp — Stress tests for lease's pooled allocator.
//
// Scenarios:
//   1. Over-capacity: create >64 CBs simultaneously (slab overflow → malloc)
//   2. Cross-thread last closer: ro on thread A, release on thread B
//   3. Mass thread create/exit: TLS cache drain + refill
//   4. TLS overflow: >128 objects in one thread (cache → slab → malloc)
//   5. Concurrent churn: multiple threads hitting the lock-free slab
//   6. Rapid create/destroy cycle (pool reuse)
//   7. Borrow across threads, release on originator

#include "lease.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace lease::access;

// ============================================================
// Test 1: Over-capacity — create >64 CBs simultaneously.
// slab capacity = 64. Creating 256 should overflow to malloc,
// then all should be reclaimed correctly.
// ============================================================
bool test_over_capacity() {
    struct widget { int v = 0; void set(int x) noexcept { v = x; } };

    constexpr int N = 256;  // 4x the slab capacity
    std::vector<widget> objs(N);
    {
        std::vector<exclusive_access<widget, read_lock>> handles;
        handles.reserve(N);

        for (int i = 0; i < N; ++i) {
            handles.emplace_back(make_rw<read_lock>(objs[i]));
            handles[i]->set(i);
        }

        for (int i = 0; i < N; ++i) {
            assert(objs[i].v == i);
        }
    }
    // All destroyed — pool should have reclaimed them

    // Re-create to verify pool reuse
    {
        std::vector<exclusive_access<widget, read_lock>> handles;
        handles.reserve(N);

        for (int i = 0; i < N; ++i) {
            handles.emplace_back(make_rw<read_lock>(objs[i]));
            handles[i]->set(i * 2);
        }

        for (int i = 0; i < N; ++i) {
            assert(objs[i].v == i * 2);
        }
    }

    return true;
}

// ============================================================
// Test 2: True cross-thread last closer.
// Create on thread A, pass to thread B, destroy on thread B.
// ============================================================
bool test_cross_thread_last_closer() {
    struct payload { int x = 99; };
    payload p;

    shared_access<payload, read_lock>* ro_ptr = nullptr;

    std::thread producer([&]() {
        auto rw = make_rw<read_lock>(p);
        auto ro = rw.borrow_ro();  // refcount = 2
        ro_ptr = new shared_access<payload, read_lock>(std::move(ro));
        // rw dies here: refcount 2->1. ro_ptr holds the last reference.
    });

    producer.join();
    assert((*ro_ptr)->x == 99);

    std::thread consumer([&]() {
        // Destroy on a different thread — last closer cross-thread
        delete ro_ptr;
    });

    consumer.join();
    return true;
}

// ============================================================
// Test 3: Mass thread create/exit.
// Many threads each create and destroy proxies. TLS caches
// drain on thread exit, returning blocks to the global slab.
// ============================================================
bool test_mass_thread_churn() {
    struct item { int v = 0; void set(int x) noexcept { v = x; } };

    constexpr int NUM_THREADS = 32;
    constexpr int ITERS_PER_THREAD = 1000;

    std::atomic<int> total_ok{0};

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&total_ok, t]() {
            item local_obj;
            for (int i = 0; i < ITERS_PER_THREAD; ++i) {
                auto rw = make_rw<read_lock>(local_obj);
                rw->set(t * 1000 + i);
                {
                    auto ro = rw.borrow_ro();
                    if (ro->v == t * 1000 + i) {
                        total_ok.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    const int expected = NUM_THREADS * ITERS_PER_THREAD;
    const int got = total_ok.load();
    assert(got == expected);
    printf("  mass_thread_churn: %d/%d OK\n", got, expected);
    return got == expected;
}

// ============================================================
// Test 4: TLS overflow — one thread creates >128 objects.
// Exercises cache -> slab -> malloc fallback.
// ============================================================
bool test_tls_overflow() {
    struct thing { int id = 0; void set(int x) noexcept { id = x; } };

    constexpr int N = 512;  // 4x cache cap, 8x slab cap
    std::vector<thing> things(N);
    {
        // First batch: create all 512
        std::vector<exclusive_access<thing, read_lock>> handles;
        handles.reserve(N);
        for (int i = 0; i < N; ++i) {
            handles.emplace_back(make_rw<read_lock>(things[i]));
            handles[i]->set(i);
        }
        for (int i = 0; i < N; ++i) {
            assert(things[i].id == i);
        }
    }
    // All destroyed — TLS cache + slab + malloc all exercised

    {
        // Second batch: re-create (pool reuse)
        std::vector<exclusive_access<thing, read_lock>> handles;
        handles.reserve(N);
        for (int i = 0; i < N; ++i) {
            handles.emplace_back(make_rw<read_lock>(things[i]));
            handles[i]->set(i * 10);
        }
        for (int i = 0; i < N; ++i) {
            assert(things[i].id == i * 10);
        }
    }
    return true;
}

// ============================================================
// Test 5: Concurrent churn — multiple threads hitting slab CAS.
// ============================================================
bool test_concurrent_churn() {
    struct node { std::atomic<int> counter{0}; };

    constexpr int NUM_THREADS = 8;
    constexpr int OBJS_PER_THREAD = 64;
    constexpr int ITERS = 500;

    std::vector<node> nodes(NUM_THREADS * OBJS_PER_THREAD);

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&nodes, t]() {
            for (int iter = 0; iter < ITERS; ++iter) {
                int idx = (t * OBJS_PER_THREAD) + (iter % OBJS_PER_THREAD);
                auto rw = make_rw<read_lock>(nodes[idx]);
                rw->counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads) th.join();

    int total = 0;
    for (auto& n : nodes) {
        total += n.counter.load();
    }
    const int expected = NUM_THREADS * ITERS;
    assert(total == expected);
    printf("  concurrent_churn: %d increments, no crash\n", total);
    return total == expected;
}

// ============================================================
// Test 6: Rapid create-destroy cycle (TLS cache fast path).
// ============================================================
bool test_rapid_cycle() {
    struct small { int x = 0; void set(int v) noexcept { x = v; } };
    small s;

    constexpr int ITERS = 100000;
    for (int i = 0; i < ITERS; ++i) {
        auto rw = make_rw<read_lock>(s);
        rw->set(i);
    }
    assert(s.x == ITERS - 1);
    return true;
}

// ============================================================
// Test 7: Borrow across threads, release on originator.
// ============================================================
bool test_borrow_and_revoke() {
    struct config { int port = 8080; };
    config cfg;

    auto rw = make_rw<read_lock>(cfg);
    rw->port = 9090;

    auto ro = rw.borrow_ro();  // refcount = 2

    std::thread reader([&ro]() {
        assert(ro->port == 9090);
    });

    reader.join();
    assert(ro->port == 9090);

    { auto ro2 = std::move(ro); }  // release ro, refcount back to 1
    rw->port = 7070;
    assert(cfg.port == 7070);

    return true;
}

// ============================================================
int main() {
    struct Test { const char* name; bool (*fn)(); };
    Test tests[] = {
        {"over_capacity",            test_over_capacity},
        {"cross_thread_last_closer", test_cross_thread_last_closer},
        {"mass_thread_churn",        test_mass_thread_churn},
        {"tls_overflow",             test_tls_overflow},
        {"concurrent_churn",         test_concurrent_churn},
        {"rapid_cycle",              test_rapid_cycle},
        {"borrow_and_revoke",        test_borrow_and_revoke},
    };

    int passed = 0;
    for (auto& t : tests) {
        printf("pool_stress: %s ... ", t.name);
        fflush(stdout);
        if (t.fn()) {
            printf("OK\n");
            ++passed;
        } else {
            printf("FAIL\n");
        }
    }

    printf("\n%d/%d pool stress tests passed\n", passed, (int)(sizeof(tests)/sizeof(tests[0])));
    return passed == (int)(sizeof(tests)/sizeof(tests[0])) ? 0 : 1;
}
