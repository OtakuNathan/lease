// bench.cpp — cost-model evidence for lease.
//
// *** TESTING SEAM ***
// This file is a measurement harness, not production code. It uses explicit
// reference captures ([&sp, &sink, ...]) to observe measurement subjects and
// accumulate results — this is measurement plumbing, not authority access.
// The lease value-semantics discipline (no reference captures for capability
// access) applies to production code, not to benchmark scaffolding.
//
// Answers two questions:
//   1. Release path: how cheap is lease compared to the usual alternatives
//      (shared_ptr, std::shared_mutex, plain reference)?
//   2. Debug checking: what does LSE_ACCESS_CHECKING actually cost?
//
// Build (Release):
//   g++ -std=c++17 -O2 -DNDEBUG -Ilease bench/bench.cpp -pthread -o bench_release
// Build (Debug):
//   g++ -std=c++17 -O2 -Ilease bench/bench.cpp -pthread -o bench_debug
//
// Numbers are ns/operation on this machine. They are relative, not absolute:
// the point is the *shape* of the cost (free vs. atomic vs. lock), not the
// cycle count.
//
// Reference hardware (this box): Raspberry Pi 4 Model B Rev 1.4,
// ARM Cortex-A72 (aarch64), 4 cores, 4GB RAM. The bare-path numbers below
// are therefore measured on a deliberately slow, low-power board — on a
// desktop x86 they come out even cheaper relative to the lock baselines.

#include "lease.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <shared_mutex>
#include <sys/utsname.h>

namespace {

struct widget {
    int value = 0;
    int get() const noexcept { return value; }
    void set(int v) noexcept { value = v; }
};

using clock_type = std::chrono::steady_clock;

template <typename F>
double measure_ns(size_t iters, F f) {
    const auto t0 = clock_type::now();
    f();
    const auto t1 = clock_type::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

volatile int g_sink = 0;

// Prevents the compiler from proving a measurement loop has no observable
// effect and deleting it (which would report a dishonest 0.00 ns/op).
#if defined(__GNUC__) || defined(__clang__)
#  define NO_FOLD asm volatile("" ::: "memory")
#else
#  define NO_FOLD
#endif

// ---------------------------------------------------------------- read path

// shared_ptr copy: two atomic RMWs per iteration (increment + decrement),
// plus an atomic-load dereference. This is the "I just want a shared read"
// baseline most people reach for first.
double bench_shared_ptr_read(size_t iters) {
    auto sp = std::make_shared<widget>();
    sp->value = 7;
    int sink = 0;
    const double ns = measure_ns(iters, [&sp, iters, &sink] {
        for (size_t i = 0; i < iters; ++i) {
            auto s = sp;        // atomic refcount increment
            sink += s->get();   // atomic load through control block
            NO_FOLD;
        }                       // atomic refcount decrement
    });
    g_sink = sink;
    return ns;
}

// lease, bare recipe: borrow + copy + read compiles to a plain member copy.
// No atomics, no heap, no locks. The same cost as passing a bare reference.
double bench_lease_bare_read(size_t iters) {
    widget w;
    auto rw = lease::access::make_rw(w);
    rw->set(7);
    int sink = 0;
    const double ns = measure_ns(iters, [&rw, iters, &sink] {
        for (size_t i = 0; i < iters; ++i) {
            auto ro = rw.borrow_ro();  // plain copy
            sink += ro->get();         // plain load
            NO_FOLD;
        }
    });
    g_sink = sink;
    return ns;
}

// lease, shared recipe: borrow + copy is one atomic fetch_add, release is
// one fetch_sub. This is the explicit price of crossing threads.
double bench_lease_shared_read(size_t iters) {
    widget w;
    auto rw = lease::access::make_rw<lease::access::shared>(w);
    rw->set(7);
    int sink = 0;
    const double ns = measure_ns(iters, [&rw, iters, &sink] {
        for (size_t i = 0; i < iters; ++i) {
            auto ro = rw.borrow_ro();  // atomic fetch_add(reader_unit)
            sink += ro->get();         // plain load
            NO_FOLD;
        }                              // atomic fetch_sub on scope exit
    });
    g_sink = sink;
    return ns;
}

// std::shared_mutex read: a real shared lock. Wait — there is no wait here
// (single thread), but the lock/unlock pair still fences and manipulates
// the mutex state machine. This is the "classic" answer to shared access.
double bench_shared_mutex_read(size_t iters) {
    widget w;
    std::shared_mutex m;
    int sink = 0;
    const double ns = measure_ns(iters, [&m, &w, iters, &sink] {
        for (size_t i = 0; i < iters; ++i) {
            std::shared_lock<std::shared_mutex> lk(m);
            sink += w.get();
            NO_FOLD;
        }
    });
    g_sink = sink;
    return ns;
}

// --------------------------------------------------------------- write path

double bench_lease_bare_write(size_t iters) {
    widget w;
    auto rw = lease::access::make_rw(w);
    const double ns = measure_ns(iters, [&rw, iters] {
        for (size_t i = 0; i < iters; ++i) {
            rw->set(static_cast<int>(i));  // plain store
            NO_FOLD;
        }
    });
    g_sink = w.value;
    return ns;
}

double bench_lease_shared_write(size_t iters) {
    widget w;
    auto rw = lease::access::make_rw<lease::access::shared>(w);
    const double ns = measure_ns(iters, [&rw, iters] {
        for (size_t i = 0; i < iters; ++i) {
            rw->set(static_cast<int>(i));  // CAS acquire + CAS release
            NO_FOLD;
        }
    });
    g_sink = w.value;
    return ns;
}

double bench_shared_mutex_write(size_t iters) {
    widget w;
    std::shared_mutex m;
    const double ns = measure_ns(iters, [&m, &w, iters] {
        for (size_t i = 0; i < iters; ++i) {
            std::unique_lock<std::shared_mutex> lk(m);
            w.set(static_cast<int>(i));
            NO_FOLD;
        }
    });
    g_sink = w.value;
    return ns;
}

// ------------------------------------------------- root creation / teardown
// Root creation exercises lineage_control (heap block), the registry, and the
// initial writer admission. Debug builds pay a global mutex + hash map entry
// per root; Release builds skip the registry entirely.

double bench_lease_root_create(size_t iters) {
    widget w;
    const double ns = measure_ns(iters, [&w, iters] {
        for (size_t i = 0; i < iters; ++i) {
            auto rw = lease::access::make_rw<lease::access::shared>(w);
            rw->set(1);
            NO_FOLD;
        }
    });
    g_sink = w.value;
    return ns;
}

double bench_shared_ptr_create(size_t iters) {
    const double ns = measure_ns(iters, [iters] {
        for (size_t i = 0; i < iters; ++i) {
            auto sp = std::make_shared<widget>();
            sp->set(1);
            NO_FOLD;
        }
    });
    g_sink = 1;
    return ns;
}

void run(const char* label, double ns) {
    static const char* mode =
#if defined(NDEBUG)
            "release";
#else
            "debug";
#endif
    std::printf("%-34s %-9s %9.2f ns/op\n", label, mode, ns);
}

} // namespace

int main() {
    constexpr size_t K = 20'000'000;

    struct utsname host;
    if (uname(&host) == 0) {
        std::printf("== lease cost model (%s build) on %s %s %s ==\n",
#if defined(NDEBUG)
                    "release",
#else
                    "debug",
#endif
                    host.sysname, host.release, host.machine);
    } else {
        std::printf("== lease cost model (%s build) ==\n",
#if defined(NDEBUG)
                    "release"
#else
                    "debug"
#endif
        );
    }
    std::printf("%-34s %-9s %12s\n", "case", "mode", "ns/op");

    run("read: shared_ptr copy", bench_shared_ptr_read(K));
    run("read: lease bare", bench_lease_bare_read(K));
    run("read: lease shared", bench_lease_shared_read(K));
    run("read: std::shared_mutex", bench_shared_mutex_read(K));

    run("write: lease bare", bench_lease_bare_write(K));
    run("write: lease shared", bench_lease_shared_write(K));
    run("write: std::shared_mutex", bench_shared_mutex_write(K));

    constexpr size_t C = 1'000'000;
    run("root create+destroy: lease", bench_lease_root_create(C));
    run("root create+destroy: shared_ptr", bench_shared_ptr_create(C));

    return 0;
}
