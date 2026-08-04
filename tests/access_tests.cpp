// access_tests.cpp — contracts and executable examples for lease.hpp.
#include "lease.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// ============================================================
namespace demo {

    using lease::access::make_ro;
    using lease::access::make_rw;
    using lease::access::probe;
    using lease::access::read_lock;
    using lease::access::shared_access;
    using lease::access::exclusive_access;

    struct widget {
        int value = 0;

        void set(int v) noexcept { value = v; }
        int get() const noexcept { return value; }
    };

// ---- Demo decorator: audit -------------------------------------------------
// Not part of the library: it is the worked example for the decorator
// extension point. Two observations:
//   * access_count() counts only write expressions, incremented inside the
//     writer exclusion (write_arrow -> mutable_write_object), so a plain
//     counter is race-free under the read_lock protocol's happens-before
//     edges; bare recipes are single-threaded by contract.
//   * active_readers()/writer_active()/root_alive() merely observe the
//     read_lock atomic word. They are not used for synchronization, so
//     memory_order_relaxed is enough — acquire would only add cost.
    template <typename Inner, typename Policy>
    class audit_impl : public Inner {
    public:
        using value_type = typename Inner::value_type;
        using policy_type = Policy;
        using Inner::Inner;

        std::size_t access_count() const noexcept { return accesses_; }

        std::size_t active_readers() const noexcept {
            if (auto* control = this->control_pointer()) {
                return lease::access::access_counter::reader_count(
                        control->state().word.load(std::memory_order_relaxed));
            }
            return 0;
        }

        bool writer_active() const noexcept {
            if (auto* control = this->control_pointer()) {
                return (control->state().word.load(std::memory_order_relaxed) &
                        lease::access::access_counter::writer_bit) != 0;
            }
            return false;
        }

        bool root_alive() const noexcept {
            if (auto* control = this->control_pointer()) {
                return (control->state().word.load(std::memory_order_relaxed) &
                        lease::access::access_counter::root_bit) != 0;
            }
            return false;
        }

    protected:
        value_type* mutable_write_object() const noexcept {
            ++accesses_;
            return Inner::mutable_write_object();
        }

        value_type* mutable_object() const noexcept {
            return Inner::mutable_object();
        }

        const value_type* const_object() const noexcept {
            return Inner::const_object();
        }

    private:
        mutable std::size_t accesses_ = 0;
    };

    struct audit {
        template <typename Inner, typename Policy>
        using apply = audit_impl<Inner, Policy>;
    };

    using plain_ro = shared_access<widget>;
    using plain_rw = exclusive_access<widget>;
    using locked_rw = exclusive_access<widget, read_lock>;
    using locked_ro = shared_access<widget, read_lock>;

    static_assert(std::is_copy_constructible<plain_ro>::value,
                  "shared_access must be copy constructible");
    static_assert(!std::is_copy_assignable<plain_ro>::value,
                  "shared_access assignment intentionally remains unavailable");
    static_assert(!std::is_copy_constructible<plain_rw>::value,
                  "exclusive_access must be unique");
    static_assert(std::is_move_constructible<plain_rw>::value,
                  "exclusive_access must transfer authority by move construction");
    static_assert(!std::is_move_assignable<plain_rw>::value,
                  "handle assignment is intentionally unavailable");

    static_assert(std::is_same<
                          decltype(std::declval<const plain_ro&>().operator->()),
                          const widget*>::value,
                  "shared_access projects shallow const T");

// ---- Decorator contract probes ---------------------------------------------
// Every decorator must satisfy the contract checked by decorator_probe.
    static_assert(lease::access::decorator_probe<read_lock, widget, lease::access::ro_tag>::value,
                  "read_lock must satisfy the decorator contract");
    static_assert(lease::access::decorator_probe<audit, widget, lease::access::rw_tag>::value,
                  "audit must satisfy the decorator contract");
    static_assert(lease::access::decorator_probe<probe, widget, lease::access::ro_tag>::value,
                  "probe must satisfy the decorator contract");

// ---- Type-list utilities (dynabridge-style flat inheritance) ---------------
    using tl = lease::access::type_list<int, double, char>;
    static_assert(lease::access::type_list_size<tl>::value == 3,
                  "type_list_size");
    static_assert(std::is_same<lease::access::element_at_t<0, tl>, int>::value,
                  "element_at<0>");
    static_assert(std::is_same<lease::access::element_at_t<1, tl>, double>::value,
                  "element_at<1>");
    static_assert(std::is_same<lease::access::element_at_t<2, tl>, char>::value,
                  "element_at<2>");
    static_assert(std::is_same<
                          lease::access::prepend_t<float, tl>,
                          lease::access::type_list<float, int, double, char>>::value,
                  "prepend_t");
    static_assert(lease::access::contains<int, tl>::value,
                  "contains finds a present type");
    static_assert(!lease::access::contains<float, tl>::value,
                  "contains rejects an absent type");

// ---- Recipe contract: explicit beats implicit ------------------------------
// A bare recipe is single-threaded and free: no read_lock, no atomics, no
// registry. read_lock is the explicit opt-in for the multi-threaded mode.

    static_assert(!lease::access::contains<read_lock,
                          shared_access<widget>::decorator_list>::value,
                  "default shared_access is free (no read_lock)");
    static_assert(!lease::access::contains<read_lock,
                          exclusive_access<widget>::decorator_list>::value,
                  "default exclusive_access is free (no read_lock)");

    static_assert(lease::access::contains<read_lock,
                          shared_access<widget, read_lock>::decorator_list>::value,
                  "explicit read_lock shared_access carries read_lock");
    static_assert(lease::access::contains<read_lock,
                          locked_rw::decorator_list>::value,
                  "explicit read_lock exclusive_access carries read_lock");

    // Writer exclusion is compiled in/out with the recipe.
    static_assert(std::is_same<
                          decltype(std::declval<exclusive_access<widget>&>().operator->()),
                          lease::access::write_arrow<widget, false>>::value,
                  "bare rw uses an empty writer scope");
    static_assert(std::is_same<
                          decltype(std::declval<locked_rw&>().operator->()),
                          lease::access::write_arrow<widget, true>>::value,
                  "read_lock rw uses atomic writer exclusion");

    // A bare rw locks into a read_lock rw; the target recipe carries read_lock.
    // lock() is callable on an lvalue (no &&): like unique_ptr::release(), the
    // destructive conversion invalidates the source handle in place.
    static_assert(std::is_same<
                          decltype(std::declval<exclusive_access<widget>&>().lock()),
                          locked_rw>::value,
                  "bare rw locks into read_lock rw");

    // A locked rw can never downgrade: no reverse conversion is declared, and
    // lock() is SFINAE'd to bare recipes only, so a locked proxy has no lock()
    // member at all — the restriction is structural, not a static_assert.

    void free_mode_test() {
        widget w;

        // Bare lineage: copies are free (no reader counting) and writes
        // bypass the atomic word. Single-thread usage by design — the default.
        {
            auto rw = make_rw(w);
            rw->set(1);

            auto ro = rw.borrow_ro();       // bare ro: no reader count
            auto ro2 = ro;                  // free copy
            assert(ro->get() == 1);
            assert(ro2->get() == 1);

            rw->set(2);                     // bare write: no CAS, no wait
            assert(ro->get() == 2);         // same referent, readers see it

            // Lock: bare rw -> read_lock rw. Destructive; the old handle hands
            // over root ownership and is invalidated. No std::move needed.
            // (The derived ro above is out of scope here — the orphan-reader
            // contract requires it destroyed before lock().)
            auto locked = rw.lock();
            assert(!rw);

            locked->set(3);                 // locked write: CAS enforced now
            auto locked_ro = locked.borrow_ro();   // locked ro: counts readers
            auto locked_ro2 = locked_ro;           // counted copy
            assert(locked_ro->get() == 3);
            assert(locked_ro2->get() == 3);
        } // locked_ro / locked_ro2 and the locked lineage close cleanly here.

        // Bare roots create no lineage control and register nothing, so the
        // Debug root-registry conflict check does not apply in free mode. That
        // is the deliberate price of the zero-cost path: the caller owns the
        // single-lineage promise until lock() re-arms the check.
        {
            auto rw = make_rw(w);
            rw->set(4);
            assert(w.value == 4);
        }

        // A bare rw can also downgrade before any lock: the bare referent
        // transfers, no control block is ever created.
        {
            auto rw = make_rw(w);
            rw->set(5);
            auto ro = rw.downgrade();
            assert(!rw);
            assert(ro->get() == 5);

            auto ro2 = ro;  // still free copies
            assert(ro2->get() == 5);
        }

        // A standalone bare shared_access root is equally free.
        {
            auto ro = make_ro(w);
            auto copy = ro;
            assert(copy->get() == 5);
        }

        std::puts("free_mode_test OK");
    }

    void object_test() {
        widget w;
        auto rw = make_rw<read_lock, audit>(w);

        rw->set(7);                        // 1st write expression (counted)
        assert(rw.clone().get() == 7);     // read path: not counted
        assert(rw.access_count() >= 1);

        {
            auto ro1 = rw.borrow_ro();     // reader acquired via protocol
            auto ro2 = ro1;                // +1 reader each
            assert(rw.active_readers() >= 2);  // observed via protocol state
            assert(rw.root_alive());
            assert(!rw.writer_active());   // no write expression in flight
            assert(ro1->get() == 7);
            assert(ro2->get() == 7);

            // rw->set(...) here would wait for ro1/ro2 and self-deadlock in this
            // thread. That is the deliberate reader-preferred runtime contract.
        }

        rw.assign(widget{11});             // 2nd write expression (counted)
        assert(rw->get() == 11);
        assert(rw.access_count() >= 2);

        auto ro = rw.downgrade();          // management op: not counted
        assert(!rw);
        assert(ro->get() == 11);
        // The downgraded root itself holds one reader share.
        assert(ro.active_readers() == 1);

        std::puts("object_test OK");
    }

    // Regression test for the shared_access::clone() self-deadlock: a reader
    // holds the reader count, so taking a writer scope from it could never
    // succeed. clone() must not attempt writer exclusion at all.
    void ro_clone_test() {
        widget w;

        {
            auto rw = make_rw<read_lock>(w);
            rw->set(31);
            auto ro = rw.borrow_ro();
            auto c = ro.clone();           // must return immediately, no deadlock
            assert(c.get() == 31);
        }

        // Bare recipe path: same call, zero atomics involved.
        {
            auto rw = make_rw(w);
            rw->set(32);
            auto ro = rw.borrow_ro();
            auto c = ro.clone();
            assert(c.get() == 32);
        }

        std::puts("ro_clone_test OK");
    }

    // The recipe is part of the type: a read_lock rw borrows a read_lock ro.
    locked_ro reader_outliving_rw(widget& w) {
        auto rw = make_rw<read_lock>(w);
        rw->set(21);
        return rw.borrow_ro();
    }

    locked_ro reader_copy_outliving_ro_root(widget& w) {
        auto root = make_ro<read_lock>(w);
        auto copy = root;
        return copy;
    }

    void last_closer_test() {
        widget w;

        {
            auto ro = reader_outliving_rw(w);
            assert(ro->get() == 21);
        } // rw root is already gone; this reader is the final closer.

        // Successful re-registration proves that the orphaned control was deleted
        // and the Debug provenance entry was removed by the last reader.
        {
            auto rw = make_rw<read_lock>(w);
            rw->set(22);
        }

        {
            auto ro = reader_copy_outliving_ro_root(w);
            assert(ro->get() == 22);
        } // copied reader closes after the original make_ro root has gone.

        {
            auto rw = make_rw<read_lock>(w);
            rw->set(23);
            assert(rw->get() == 23);
        }

        std::puts("last_closer_test OK");
    }

    void shallow_span_test() {
        std::vector<int> values{1, 2, 3, 4, 5};
        lease::span<int> whole(values.data(), values.size());

        // Proxy governs the span descriptor object, not the represented elements.
        auto rw = make_rw(whole);
        lease::span<int> tail = rw->split_off(2);

        assert(whole.size() == 2);
        assert(tail.size() == 3);

        // tail is a new span object, therefore it may have its own rw lineage.
        auto tail_rw = make_rw(tail);
        tail_rw->operator[](0) = 30;
        assert(values[2] == 30);

        {
            auto whole_ro = rw.borrow_ro();

            // Shallow const is intentional: const span<int> still exposes int&.
            whole_ro->operator[](0) = 10;
            assert(values[0] == 10);
        }

        std::puts("shallow_span_test OK");
    }

    void concurrency_test() {
        widget w;
        // Multi-threading is explicit: only a read_lock recipe may cross
        // threads. A bare recipe here would be a silent data race — the whole
        // point of "explicit beats implicit".
        auto rw = make_rw<read_lock>(w);

        constexpr int iterations = 20000;
        std::atomic<std::int64_t> checksum{0};

        std::thread writer([&] {
            for (int i = 0; i < iterations; ++i) {
                rw->set(i);
            }
        });

        std::vector<std::thread> readers;
        for (int t = 0; t < 3; ++t) {
            readers.emplace_back([&] {
                for (int i = 0; i < iterations; ++i) {
                    auto ro = rw.borrow_ro();
                    checksum.fetch_add(ro->get(), std::memory_order_relaxed);
                }
            });
        }

        writer.join();
        for (auto& thread : readers) {
            thread.join();
        }

        assert(checksum.load(std::memory_order_relaxed) >= 0);
        std::puts("concurrency_test OK");
    }

} // namespace demo

int main() {
    demo::free_mode_test();
    demo::object_test();
    demo::ro_clone_test();
    demo::last_closer_test();
    demo::shallow_span_test();
    demo::concurrency_test();

#if LSE_ACCESS_CHECKING && defined(LSE_ACCESS_VIOLATION)
    demo::widget w;
    auto first = lease::access::make_rw<lease::access::read_lock>(w);
    auto second = lease::access::make_ro<lease::access::read_lock>(w); // independent root: abort in Debug
    (void)first;
    (void)second;
#endif

    return 0;
}
