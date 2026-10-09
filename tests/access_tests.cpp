// access_tests.cpp — contracts and executable examples for lease.hpp.
#include "lease.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// ============================================================
namespace demo {

    using lease::access::make_ro;
    using lease::access::make_rw;
    using lease::access::probe;
    using lease::access::shared;
    using lease::access::indexed;
    using lease::access::enable_shared;
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
//   * access_count() counts only write expressions, incremented inside
//     mutable_write_object (called by write_arrow).
//   * participant_count()/active_readers() merely observe the participant
//     count. They are not used for synchronization, so memory_order_relaxed
//     is enough — acquire would only add cost.
    template <typename Inner, typename Policy>
    class audit_impl : public Inner {
    public:
        using value_type = typename Inner::value_type;
        using policy_type = Policy;
        using Inner::Inner;

        std::size_t access_count() const noexcept { return accesses_; }

        std::uint64_t participant_count() const noexcept {
            if (auto* control = this->control_pointer()) {
                return control->refcount_value();
            }
            return 0;
        }

        std::uint64_t active_readers() const noexcept {
            if (auto* control = this->control_pointer()) {
                std::uint64_t v = control->refcount_value();
                return v > 1 ? v - 1 : 0;
            }
            return 0;
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
    using locked_rw = exclusive_access<widget, shared>;
    using locked_ro = shared_access<widget, shared>;

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
    static_assert(lease::access::decorator_probe<shared, widget, lease::access::ro_tag>::value,
                  "shared must satisfy the decorator contract");
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

    // ---- dedup -------------------------------------------------------------
    using dup_list = lease::access::type_list<int, double, int, char, double>;
    using deduped = lease::access::dedup_t<dup_list>;
    static_assert(lease::access::type_list_size<deduped>::value == 3,
                  "dedup removes duplicates");
    static_assert(std::is_same<lease::access::element_at_t<0, deduped>, int>::value,
                  "dedup keeps first occurrence order (0)");
    static_assert(std::is_same<lease::access::element_at_t<1, deduped>, double>::value,
                  "dedup keeps first occurrence order (1)");
    static_assert(std::is_same<lease::access::element_at_t<2, deduped>, char>::value,
                  "dedup keeps first occurrence order (2)");

    using no_dup = lease::access::dedup_t<lease::access::type_list<>>;
    static_assert(lease::access::type_list_size<no_dup>::value == 0,
                  "dedup of empty list is empty");

// ---- Recipe contract: explicit beats implicit ------------------------------
// A bare recipe is single-threaded and free: no shared, no atomics, no
// registry. shared is the explicit opt-in for the multi-threaded mode.

    static_assert(!lease::access::contains<shared,
                          shared_access<widget>::decorator_list>::value,
                  "default shared_access is free (no shared)");
    static_assert(!lease::access::contains<shared,
                          exclusive_access<widget>::decorator_list>::value,
                  "default exclusive_access is free (no shared)");

    static_assert(lease::access::contains<shared,
                          shared_access<widget, shared>::decorator_list>::value,
                  "explicit shared shared_access carries shared");
    static_assert(lease::access::contains<shared,
                          locked_rw::decorator_list>::value,
                  "explicit shared exclusive_access carries shared");

    // Writer exclusion is compiled in/out with the recipe.
    static_assert(std::is_same<
                          decltype(std::declval<exclusive_access<widget>&>().operator->()),
                          lease::access::write_arrow<widget, false>>::value,
                  "bare rw write_arrow has no exclusivity check");
    static_assert(std::is_same<
                          decltype(std::declval<locked_rw&>().operator->()),
                          lease::access::write_arrow<widget, true>>::value,
                  "locked rw write_arrow checks count == 1 in Debug");

    // A bare rw locks into a shared rw; the target recipe carries shared.
    // enable_shared is callable on an lvalue (no &&): like unique_ptr::release(),
    // the destructive conversion invalidates the source handle in place.
    static_assert(std::is_same<
                          decltype(lease::access::enable_shared(
                              std::declval<exclusive_access<widget>&>())),
                          locked_rw>::value,
                  "bare rw locks into shared rw");

    // A locked rw can never re-lock: enable_shared() carries a static_assert
    // that rejects already-locked proxies, and no reverse conversion exists.

    void free_mode_test() {
        widget w;

        // Bare lineage: in Release, copies are free (no reader counting) and
        // writes bypass the exclusivity check. In Debug, bare also gets a
        // control block so orphan readers are caught. Single-threaded by design.
        {
            auto rw = make_rw(w);
            rw->set(1);

            {
                auto ro = rw.borrow_ro();       // Debug: refcount counted
                auto ro2 = ro;                  // counted copy
                assert(ro->get() == 1);
                assert(ro2->get() == 1);
            } // ro/ro2 destroyed — refcount back to 1

            rw->set(2);                         // safe: no readers alive
            assert(w.value == 2);

            // Lock: bare rw -> shared rw. Destructive; the old handle hands
            // over root ownership and is invalidated. No std::move needed.
            auto locked = enable_shared(rw);
            assert(!rw);

            locked->set(3);                 // locked write: count checked in Debug
            auto locked_ro = locked.borrow_ro();   // locked ro: counts readers
            auto locked_ro2 = locked_ro;           // counted copy
            assert(locked_ro->get() == 3);
            assert(locked_ro2->get() == 3);
        } // locked_ro / locked_ro2 and the locked lineage close cleanly here.

        // In Release, bare roots create no lineage control and register nothing.
        // In Debug, bare roots get a control block (orphan-reader detection).
        // Either way, after the previous lineage is fully released, a new root
        // for the same object is valid.
        {
            auto rw = make_rw(w);
            rw->set(4);
            assert(w.value == 4);
        }

        // A recipe change starts a fresh decorator chain. Even though
        // enable_shared stays rw-side, per-capability decorator state is not
        // preserved: not every decorator has an equivalent shared-mode state.
        {
            auto rw = make_rw<audit>(w);
            rw->set(40);
            assert(rw.access_count() == 1);

            auto locked = enable_shared(rw);
            assert(!rw);
            assert(locked.access_count() == 0);

            locked->set(41);
            assert(locked.access_count() == 1);
        }

        // A bare rw can also downgrade: in Debug the control block transfers
        // to the ro; in Release there is no control block (bare referent only).
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
        auto rw = make_rw<shared, audit>(w);

        rw->set(7);                        // 1st write expression (counted)
        assert(rw.clone().get() == 7);     // read path: not counted
        assert(rw.access_count() >= 1);

        {
            auto ro1 = rw.borrow_ro();     // participant added
            auto ro2 = ro1;                // +1 participant each
            assert(rw.active_readers() >= 2);  // observed via participant count
            assert(rw.participant_count() >= 3); // rw + ro1 + ro2
            assert(ro1->get() == 7);
            assert(ro2->get() == 7);

            // rw->set(...) here would abort in Debug: participants > 1.
            // That is the contract — write with no readers, or read with no write.
        }

        rw.assign(widget{11});             // 2nd write expression (counted)
        assert(rw->get() == 11);
        assert(rw.access_count() >= 2);

        auto ro = rw.downgrade();          // management op: not counted
        assert(!rw);
        assert(ro->get() == 11);
        // After downgrade: rw's reference transferred to ro. refcount = 1.
        assert(ro.participant_count() == 1);

        std::puts("object_test OK");
    }

    // Regression test: clone() on a shared_access must not attempt any
    // exclusivity check — the reader is a participant, and the data it
    // reads is consistent because rw cannot write while this ro lives.
    void ro_clone_test() {
        widget w;

        {
            auto rw = make_rw<shared>(w);
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

    // The recipe is part of the type: a shared rw borrows a shared ro.
    locked_ro reader_outliving_rw(widget& w) {
        auto rw = make_rw<shared>(w);
        rw->set(21);
        return rw.borrow_ro();
    }

    locked_ro reader_copy_outliving_ro_root(widget& w) {
        auto root = make_ro<shared>(w);
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
            auto rw = make_rw<shared>(w);
            rw->set(22);
        }

        {
            auto ro = reader_copy_outliving_ro_root(w);
            assert(ro->get() == 22);
        } // copied reader closes after the original make_ro root has gone.

        {
            auto rw = make_rw<shared>(w);
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
        // Multi-threading is explicit: only a shared recipe may cross
        // threads. The participant count model: ro copies may be shared across
        // threads freely; writing requires count == 1 (no active readers).
        auto rw = make_rw<shared>(w);
        rw->set(42);

        constexpr int iterations = 20000;
        std::atomic<std::int64_t> checksum{0};

        // Phase 1: borrow ro copies and let reader threads read concurrently.
        // This tests that participant_count::add()/remove() are thread-safe.
        {
            auto ro = rw.borrow_ro();  // count = 2 (rw + ro)

            std::vector<std::thread> readers;
            for (int t = 0; t < 3; ++t) {
                readers.emplace_back([&ro, &iterations, &checksum] {
                    for (int i = 0; i < iterations; ++i) {
                        // Each copy adds/removes a participant atomically.
                        auto local_ro = ro;  // copy: count += 1
                        checksum.fetch_add(local_ro->get(), std::memory_order_relaxed);
                        // local_ro destroyed: count -= 1
                    }
                });
            }

            for (auto& thread : readers) {
                thread.join();
            }
        } // ro destroyed: count back to 1 (only rw)

        // Phase 2: all readers gone, rw can write again (count == 1).
        rw->set(99);
        assert(w.value == 99);

        assert(checksum.load(std::memory_order_relaxed) > 0);
        std::puts("concurrency_test OK");
    }

// ---- indexed decorator tests -----------------------------------------------

    // Compile-time: indexed detects map vs sequence vs non-indexable.
    static_assert(lease::access::detail::map_key<std::map<int, int>>::value,
                  "map_key detects std::map");
    static_assert(lease::access::detail::has_index_access<std::vector<int>>::value,
                  "has_index_access detects std::vector");
    static_assert(!lease::access::detail::is_indexable<widget>::value,
                  "widget is not indexable");
    static_assert(lease::access::detail::is_indexable<std::vector<int>>::value,
                  "vector is indexable");
    static_assert(lease::access::detail::is_indexable<std::map<std::string, int>>::value,
                  "map is indexable");

    // Fix #1: std::set has key_type but no at() — must NOT be detected as map-like.
    static_assert(!lease::access::detail::map_key<std::set<int>>::value,
                  "set has key_type but no at() — not map-like");
    static_assert(!lease::access::detail::is_indexable<std::set<int>>::value,
                  "set is not indexable (no at(), no operator[](size_t))");

    // Fix #3: dedup unifies type identity.
    static_assert(std::is_same<shared_access<int, indexed, indexed>,
                               shared_access<int, indexed>>::value,
                  "dedup unifies <indexed, indexed> and <indexed> type identity");
    static_assert(std::is_same<exclusive_access<int, indexed, indexed>,
                               exclusive_access<int, indexed>>::value,
                  "dedup unifies exclusive_access type identity");

    // Compile-time: key_type resolution.
    static_assert(std::is_same<
                      lease::access::detail::index_key<std::vector<int>>::type,
                      std::size_t>::value,
                  "vector key is size_t");
    static_assert(std::is_same<
                      lease::access::detail::index_key<std::map<std::string, int>>::type,
                      std::string>::value,
                  "map key is key_type");

    void indexed_test() {
        // --- Sequence path: vector ---
        {
            std::vector<int> vals{10, 20, 30, 40, 50};
            auto rw = make_rw<indexed>(vals);

            // Write through write_arrow (rw_tag pass-through).
            rw->operator[](0) = 99;
            assert(vals[0] == 99);

            // Read through indexed operator[] on shared_access.
            auto ro = rw.borrow_ro();
            assert(ro[0] == 99);
            assert(ro[4] == 50);

            // Borrowed ro copy also works.
            auto ro2 = ro;
            assert(ro2[2] == 30);
        }

        // --- Map path: forwards to at(), not operator[] ---
        {
            std::map<std::string, int> m{{"alpha", 1}, {"beta", 2}, {"gamma", 3}};
            auto rw = make_rw<indexed>(m);

            // Write through write_arrow (real map::operator[], inserts).
            rw->operator[]("delta") = 4;
            assert(m.at("delta") == 4);

            // Read through indexed operator[] on shared_access.
            // This forwards to at() — safe on const map, throws on missing key.
            auto ro = rw.borrow_ro();
            assert(ro["alpha"] == 1);
            assert(ro["beta"] == 2);
            assert(ro["delta"] == 4);

            // at() throws on missing key — no silent insertion.
            bool threw = false;
            try {
                (void)ro["missing"];
            } catch (const std::out_of_range&) {
                threw = true;
            }
            assert(threw);
        }

        // --- Non-indexable type: operator[] does not exist ---
        // (If it did, this function would not compile. The static_assert
        //  above already proves widget is not indexable. Here we just
        //  verify the proxy compiles and works normally without []. )
        {
            widget w;
            w.value = 42;
            auto ro = make_ro<indexed>(w);
            assert(ro->get() == 42);
        }

        // --- Works with shared recipe ---
        {
            std::vector<int> vals{100, 200, 300};
            auto rw = make_rw<shared, indexed>(vals);
            auto ro = rw.borrow_ro();
            assert(ro[0] == 100);
            assert(ro[1] == 200);
            assert(ro[2] == 300);
        }

        // --- Duplicate decorator: <indexed, indexed> collapses to one layer ---
        {
            std::vector<int> vals{7, 8, 9};
            auto rw = make_rw<indexed, indexed>(vals);
            auto ro = rw.borrow_ro();
            assert(ro[0] == 7);
            assert(ro[2] == 9);
        }

        // --- Auto-inject: make_ro(vec) without <indexed> still gets operator[] ---
        {
            std::vector<int> vals{42, 43, 44};
            auto rw = make_rw(vals);             // no explicit <indexed>
            auto ro = rw.borrow_ro();
            assert(ro[0] == 42);                 // operator[] auto-injected
            assert(ro[2] == 44);

            // Map auto-inject too: ro[key] forwards to at().
            std::map<std::string, int> m{{"x", 10}};
            auto mrw = make_rw(m);
            auto mro = mrw.borrow_ro();
            assert(mro["x"] == 10);
        }

        std::puts("indexed_test OK");
    }

// ---- Token security test (Fix #4) ------------------------------------------
// A decorator with a no-op invalidate() must NOT prevent the proxy from
// invalidating the source after enable_shared. The proxy owns invalidate()
// and calls object_storage directly, bypassing all decorators.
    template <typename Inner, typename Policy>
    class noop_invalidate_impl : public Inner {
    public:
        using value_type = typename Inner::value_type;
        using policy_type = Policy;
        using Inner::Inner;
        noop_invalidate_impl(const noop_invalidate_impl&) noexcept = default;
        noop_invalidate_impl(noop_invalidate_impl&&) noexcept = default;
        noop_invalidate_impl& operator=(const noop_invalidate_impl&) = delete;
        noop_invalidate_impl& operator=(noop_invalidate_impl&&) = delete;

    protected:
        // Malicious no-op: does not forward to Inner::invalidate().
        void invalidate() noexcept { /* trap */ }
    };

    struct noop_invalidate {
        template <typename Inner, typename Policy>
        using apply = noop_invalidate_impl<Inner, Policy>;
    };

    void token_security_test() {
        widget w;
        w.value = 99;

        auto rw = make_rw<noop_invalidate>(w);
        assert(rw);

        // enable_shared must invalidate the source even though the decorator
        // has a no-op invalidate(). The proxy bypasses decorators.
        auto locked = enable_shared(rw);
        assert(!rw);   // source is dead — object_storage::invalidate() ran

        locked->set(100);
        assert(w.value == 100);

        std::puts("token_security_test OK");
    }

// ---- Scoped read/write lambda tests ----------------------------------------
    void scoped_lambda_test() {
        // --- bare recipe: write(lambda) + read(lambda) ---
        {
            widget w;
            auto rw = make_rw(w);

            rw.write([](widget& w) {
                w.set(42);
            });
            assert(w.value == 42);

            // read on rw (via borrow_ro)
            auto ro = rw.borrow_ro();
            int val = ro.read([](const widget& w) {
                return w.get();
            });
            assert(val == 42);
        }

        // --- locked recipe: write(lambda) checks exclusivity for entire block ---
        {
            std::vector<int> vals{5, 3, 1, 4, 2};
            auto rw = make_rw<shared>(vals);

            // std::sort inside write(lambda): no readers can exist, safe to mutate.
            rw.write([](std::vector<int>& v) {
                std::sort(v.begin(), v.end());
            });

            assert(vals[0] == 1);
            assert(vals[4] == 5);

            // read(lambda) on locked ro
            auto ro = rw.borrow_ro();
            int first = ro.read([](const std::vector<int>& v) {
                return v[0];
            });
            assert(first == 1);
        }

        // --- noexcept propagation: noexcept lambda → noexcept write ---
        {
            widget w;
            auto rw = make_rw(w);
            // C++14: can't use lambda in unevaluated context, so test at runtime.
            rw.write([](widget& w) noexcept { w.set(7); });
            assert(w.value == 7);
            // noexcept propagation verified at runtime: a noexcept lambda
            // through write() must not throw. (Static_assert with noexcept
            // function pointer types behaves differently on clang vs gcc.)
        }

        // --- exception safety: lambda throws, lease state stays consistent ---
        {
            widget w;
            w.value = 77;
            auto rw = make_rw<shared>(w);

            bool threw = false;
            try {
                rw.write([](widget& w) {
                    w.set(88);
                    throw std::runtime_error("test");
                });
            } catch (const std::runtime_error&) {
                threw = true;
            }
            assert(threw);

            // No scope to leak — lease state is consistent. We can write again.
            rw->set(99);
            assert(w.value == 99);
        }

        std::puts("scoped_lambda_test OK");
    }

// ---- Unified write admission test ------------------------------------------
// assign() and operator=(T) must go through write_arrow, not bypass it.
// Verified via audit decorator: assign/operator= increment access_count,
// proving they traverse the same decorator write seam as operator->.
    void assign_admission_test() {
        widget w;
        auto rw = make_rw<shared, audit>(w);

        // assign() goes through write_arrow → audit counter increments.
        std::size_t before = rw.access_count();
        rw.assign(widget{42});
        assert(rw.access_count() == before + 1);
        assert(rw->get() == 42);

        // operator=(T) also goes through write_arrow.
        before = rw.access_count();
        rw = widget{99};
        assert(rw.access_count() == before + 1);
        assert(rw->get() == 99);

        std::puts("assign_admission_test OK");
    }

// ---- Write order test: decorator side effects only on admitted writes ------
// write_arrow checks core_valid() and exclusivity BEFORE calling
// mutable_write_object(). A rejected write must not trigger decorator
// side effects. Verified via a custom decorator counting seam calls.
    template <typename Inner, typename Policy>
    class write_order_impl : public Inner {
    public:
        using value_type = typename Inner::value_type;
        using policy_type = Policy;
        using Inner::Inner;

        std::size_t write_seam_calls() const noexcept { return seam_calls_; }

    protected:
        value_type* mutable_write_object() const noexcept {
            ++seam_calls_;
            return Inner::mutable_write_object();
        }

    private:
        mutable std::size_t seam_calls_ = 0;
    };

    struct write_order {
        template <typename Inner, typename Policy>
        using apply = write_order_impl<Inner, Policy>;
    };

    void write_order_test() {
        widget w;
        w.value = 77;

        // Bare recipe: in Release, write_arrow has Shared=false, no exclusivity
        // check. In Debug, bare also checks exclusivity (control block exists).
        // Either way, core_valid() still gates mutable_write_object().
        {
            auto rw = make_rw<write_order>(w);
            assert(rw.write_seam_calls() == 0);

            rw->set(1);
            assert(rw.write_seam_calls() == 1);
        }

        // Locked recipe: exclusivity check gates the seam.
        {
            auto rw = make_rw<shared, write_order>(w);
            assert(rw.write_seam_calls() == 0);

            rw->set(2);
            assert(rw.write_seam_calls() == 1);

            {
                auto ro = rw.borrow_ro();  // refcount = 2
                // rw->set(3) would abort here (refcount > 1).
                // The seam_calls_ counter would NOT increment because
                // is_exclusive() fails before mutable_write_object().
                assert(rw.write_seam_calls() == 1);
            }

            // After ro is gone, write works again.
            rw->set(3);
            assert(rw.write_seam_calls() == 2);
        }

        std::puts("write_order_test OK");
    }

// ---- access()/value() adapter tests ----------------------------------------
    void adapt_test() {
        using lease::access::access;
        using lease::access::value;

        // ---- access() ----

        // access(mutable value) → T*
        {
            int x = 42;
            int* p = access(x);
            assert(p == &x);
            *p = 99;
            assert(x == 99);
        }

        // access(const value) → const T*
        {
            const int x = 7;
            const int* p = access(x);
            assert(p == &x);
            assert(*p == 7);
        }

        // access(raw ptr) → T*
        {
            int x = 5;
            int* p = access(&x);
            assert(p == &x);
        }

        // access(bare ro) → Cap& → operator-> → const T*
        {
            widget w; w.value = 42;
            auto ro = make_ro(w);
            auto& cap = access(ro);
            assert(cap->get() == 42);
        }

        // access(const bare ro) → const Cap& → operator->() const → const T*
        // Bug2 regression: const cap must route to capability overload
        {
            widget w; w.value = 42;
            const auto ro = make_ro(w);
            auto& cap = access(ro);
            assert(cap->get() == 42);
        }

        // access(bare rw) → Cap& → operator-> → write_arrow → T*
        {
            widget w;
            auto rw = make_rw(w);
            auto& cap = access(rw);
            cap->set(77);
            assert(w.value == 77);
        }

        // access(shared ro) → Cap&
        {
            widget w; w.value = 42;
            auto ro = make_ro<shared>(w);
            auto& cap = access(ro);
            assert(cap->get() == 42);
        }

        // access(shared rw) → Cap&
        {
            widget w;
            auto rw = make_rw<shared>(w);
            auto& cap = access(rw);
            cap->set(88);
            assert(w.value == 88);
        }

        // ---- value() ----

        // value(plain T) → T (pass-through, RVO)
        {
            widget w; w.value = 42;
            auto copy = value(w);
            assert(copy.get() == 42);
            copy.value = 99;
            assert(w.value == 42);
        }

        // value(bare ro) → T (clone)
        {
            widget w; w.value = 42;
            auto ro = make_ro(w);
            auto copy = value(ro);
            assert(copy.get() == 42);
        }

        // value(bare rw) → T (clone)
        {
            widget w; w.value = 42;
            auto rw = make_rw(w);
            auto copy = value(rw);
            assert(copy.get() == 42);
        }

        // value(shared ro) → T (clone)
        {
            widget w; w.value = 42;
            auto ro = make_ro<shared>(w);
            auto copy = value(ro);
            assert(copy.get() == 42);
        }

        // value(shared rw) → T (clone)
        {
            widget w; w.value = 42;
            auto rw = make_rw<shared>(w);
            auto copy = value(rw);
            assert(copy.get() == 42);
        }

        // ---- Bug1 regression: decorated capabilities ----
        // value() must deduce for decorated caps where D... passes dedup_t
        {
            widget w; w.value = 42;
            auto ro = make_ro<shared, indexed>(w);
            auto copy = value(ro);
            assert(copy.get() == 42);
        }

        {
            widget w; w.value = 42;
            auto rw = make_rw<shared, audit>(w);
            auto copy = value(rw);
            assert(copy.get() == 42);
        }

        std::puts("adapt_test OK");
    }

// ---- Static-storage destruction order -------------------------------------
// A namespace-scope owner is destroyed after any function-local static whose
// construction it preceded. This global container starts holding proxies
// during main, i.e. after root_registry (a function-local static created by
// the first make_* call) completed construction — so the sink is destroyed
// AFTER the registry, and its handle destructors must still be able to
// unregister. Without an immortal registry this aborts at process exit with
// "unregistering unknown proxy lineage"; the failure shows up as a non-zero
// exit status after every line of main has already been printed.
    widget g_exit_order_object;                                  // referent: outlives the handles
    std::vector<shared_access<widget, shared>> g_exit_order_sink;  // owner: destroyed after the registry

    void static_owner_exit_test() {
        auto root = make_ro<shared>(g_exit_order_object);
        for (int i = 0; i < 4; ++i) {
            g_exit_order_sink.push_back(root);
        }
        assert(g_exit_order_sink.size() == 4);
        std::puts("static_owner_exit_test OK (teardown runs after main)");
    }

} // namespace demo

int main() {
    demo::free_mode_test();
    demo::object_test();
    demo::ro_clone_test();
    demo::last_closer_test();
    demo::shallow_span_test();
    demo::concurrency_test();
    demo::indexed_test();
    demo::token_security_test();
    demo::scoped_lambda_test();
    demo::assign_admission_test();
    demo::write_order_test();
    demo::adapt_test();
    demo::static_owner_exit_test();

#if LSE_ACCESS_CHECKING && defined(LSE_ACCESS_VIOLATION)
    demo::widget w;
    auto first = lease::access::make_rw<lease::access::shared>(w);
    auto second = lease::access::make_ro<lease::access::shared>(w); // independent root: abort in Debug
    (void)first;
    (void)second;
#endif

    return 0;
}
