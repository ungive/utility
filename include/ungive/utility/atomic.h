#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

// Track get() lifetimes during debugging.
#if !defined(NDEBUG) && !defined(NO_TRACK_LIFETIMES)
#define TRACK_LIFETIMES
#include <set>
#endif

// Enable lifetime recording with unit tests.
#if defined(TRACK_LIFETIMES) && defined(UNGIVE_UTILITY_TEST)
#define LIFETIME_RECORDING
#include <deque>
#endif

// Track wait codepaths with unit tests.
#if defined(UNGIVE_UTILITY_TEST)
#define WAIT_CODEPATHS
#include <unordered_set>
#endif

namespace ungive
{
namespace utility
{

/**
 * @brief Wraps a value that can be updated atomically from multiple threads.
 *
 * Values can be retrieved by pointer (without copying) and
 * can be updated whenever there is no pointer to it in active use.
 * Whenever a pointer to the value is retrieved via Atomic::get
 * any calls to Atomic::set block until all returned pointers
 * from Atomic::get have been destructed.
 *
 * The default get lifetime is sufficiently high to loosely guarantee that
 * Atomic::set will never throw an exception when pointers returned by
 * Atomic::get are used in the way that is required in its documentation.
 *
 * @tparam T The value type.
 * @tparam DefaultGetLifetimeMillis The default lifetime
 * for pointers returned by Atomic::get.
 */
template <typename T, size_t DefaultGetLifetimeMillis = 10000>
class Atomic
{
private:
    using self_type = Atomic<T, DefaultGetLifetimeMillis>;

    static constexpr inline bool valid_lifetime(
        std::chrono::milliseconds const& lifetime)
    {
        return lifetime > std::chrono::milliseconds::zero();
    }

    static_assert(Atomic::valid_lifetime(
        std::chrono::milliseconds{ DefaultGetLifetimeMillis }));

    // Represents a destructor for a value returned by Atomic::get.
    // The destructor is a callable that is only called when enabled by a flag
    // and is accompanied by a shared pointer to the atomically wrapped value.
    struct GetDestructor
    {
        GetDestructor(std::function<void()> callable,
            std::shared_ptr<std::atomic<bool>> flag,
            std::shared_ptr<const T> value)
            : m_callable{ callable }, m_flag{ flag }, m_value_ref{ value }
        {
            assert(m_callable != nullptr);
            assert(m_flag != nullptr);
        }

        inline void operator()() const
        {
            if (m_flag->load()) {
                m_callable();
            }
        }

    private:
        const std::shared_ptr<std::atomic<bool>> m_flag{ nullptr };
        const std::function<void()> m_callable{ nullptr };
        const std::shared_ptr<const T> m_value_ref{ nullptr };
    };

    Atomic(std::shared_ptr<T>&& ptr) : m_value{ std::move(ptr) }
    {
#ifdef TRACK_LIFETIMES
        m_lifetime_thread = std::thread(&Atomic::track_lifetimes, this);
#endif
    }

public:
    // The type of the stored value.
    using value_type = T;

    // The default lifetime for returned get values.
    static constexpr auto default_get_lifetime =
        std::chrono::milliseconds{ DefaultGetLifetimeMillis };

    Atomic() : Atomic(std::make_shared<T>()) {}

    Atomic(T&& value) : Atomic(std::make_shared<T>(std::move(value))) {}

    Atomic(T const& value) : Atomic(std::make_shared<T>(value)) {}

    template <typename... Args>
    Atomic(Args&&... args)
        : Atomic(std::make_shared<T>(std::forward<Args>(args)...))
    {
    }

    ~Atomic()
    {
        // None of the destructors from references returned by get() should
        // be called anymore, since this class instance is now destroyed.
        m_active->store(false);

#ifdef TRACK_LIFETIMES
        stop_lifetime_thread();

#ifdef LIFETIME_RECORDING
        // There should be no errors left in the history.
        assert(m_lifetime_history.empty());
#endif // LIFETIME_RECORDING
#endif // TRACK_LIFETIMES
    }

    /**
     * @brief Returns a const value pointer and increments the reference count.
     *
     * The reference count is decremented when the value pointer is destructed.
     * It is recommended to not store the returned shared pointer
     * for a long time and instead only treat it as a temporary rvalue.
     * Storing it for a longer than needed is considered bad pratice
     * and may cause substantial delays when calling Atomic::set.
     *
     * The returned pointer is expected to be destructed after
     * the amount of time in Atomic::default_get_lifetime has elapsed.
     * If it is used for longer, calls to Atomic::set may throw an exception.
     * Never store a reference to the value that is pointed to by the
     * returned pointer, as that eliminates any thread-safety guarantees.
     * The value that is pointed to by the returned value must not be modified.
     * To modify the stored value use the Atomic::set method instead.
     *
     * @returns A reference-counted pointer to the internally stored value.
     */
    inline std::shared_ptr<const T> get()
    {
        return internal_get(Atomic::default_get_lifetime);
    }

    /**
     * @brief Returns a const value pointer and increments the reference count.
     *
     * The returned pointer is expected to be destructed after
     * the amount of time in the passed lifetime has elapsed.
     * The passed lifetime must be greater than zero.
     *
     * @see Atomic::get
     *
     * @param lifetime How long the returned value will be used at most.
     *
     * @returns A reference-counted pointer to the internally stored value.
     */
    std::shared_ptr<const T> get(std::chrono::milliseconds lifetime)
    {
        if (!valid_lifetime(lifetime)) {
            throw std::invalid_argument(
                "the lifetime must be greater than zero");
        }
        return internal_get(lifetime);
    }

    /**
     * @brief Atomically sets the internal value to the given value.
     *
     * Blocks until all references returned from Atomic::get are destroyed
     * or the operation times out after the given timeout duration.
     *
     * This operation is guaranteed to set the value to the one that has
     * been passed to the most recent set call. When multiple set calls
     * are blocking at the same time, the most recent call succeeds.
     *
     * Returns whether the value has been set or another more recent
     * Action::set call has set a different value from another thread.
     * When false is returned, the value may be different from the passed on.
     *
     * @param value The value to set.
     *
     * @returns A boolean indicating whether the given value was set.
     *
     * @throws std::runtime_error when a reference returned
     * by Atomic::get is used beyond it's promised lifetime.
     */
    inline bool set(T&& value) { return internal_set(std::move(value)); }

    /**
     * @brief Atomically sets the internal value to the given value.
     *
     * @see Atomic::set
     *
     * @param args Constructor arguments for the value.
     *
     * @returns A boolean indicating whether the given value was set.
     */
    template <typename... Args>
    inline bool set(Args&&... args)
    {
        return internal_set(T(std::forward<Args>(args)...));
    }

    /**
     * @brief Sets a callback for when the value is changed with Atomic::set.
     *
     * The callback must not call any instance methods of this class.
     *
     * @param callback Function that should be called on value changes.
     */
    void watch(std::function<void(T const&)> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback = callback;
    }

private:
    using clock = std::chrono::steady_clock;

    std::shared_ptr<const T> internal_get(std::chrono::milliseconds lifetime)
    {
        assert(valid_lifetime(lifetime));

        std::lock_guard<std::mutex> lock(m_mutex);
        m_refs += 1;
        auto timepoint = clock::now() + lifetime;
        m_set_deadline = std::max(m_set_deadline, timepoint);

#ifdef TRACK_LIFETIMES
        auto destructor =
            std::bind(&self_type::get_dtor_tracking, this, timepoint);
        {
            std::lock_guard<std::mutex> lock(m_lifetime_mutex);
            m_lifetime_expirations.insert(timepoint);
            m_lifetime_update = true;
            m_lifetime_cv.notify_all();
        }
#else
        auto destructor = std::bind(&self_type::get_dtor, this);
#endif // TRACK_LIFETIMES

        // Store a copy of the internal shared pointer alongside
        // the manually wrapped raw pointer so that the reference count
        // is still incremented, we can set a custom destructor here and
        // the raw pointer is guaranteed to never point to deleted memory.
        // Additionally the active flag ensures that the destructor is only
        // called while this class instance has not been destructed.
        return std::shared_ptr<T>(m_value.get(),
            std::bind(GetDestructor(destructor, m_active, m_value)));
    }

    enum class WaitResult
    {
        Ok,
        Outdated,
        Timeout
    };

    bool internal_set(T&& value)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        WaitResult result = wait(lock);
        switch (result) {
        case WaitResult::Ok:
            break;
        case WaitResult::Outdated:
            return false;
        case WaitResult::Timeout:
            throw std::runtime_error("a reference is used beyond its lifetime");
        default:
            assert(false);
            throw std::runtime_error("impossible case");
        }
        *m_value = std::move(value);
        if (m_callback) {
            m_callback(*m_value);
        }
        m_set_latest = clock::time_point::min();
        m_set_cv.notify_all();
        return true;
    }

#ifdef UNGIVE_UTILITY_TEST
    // Only make this enum visible in tests.
public:
#else
private:
#endif
    // This should get optimized away in non-test builds,
    // since neither the type nor its values are used anywhere.
    enum class WaitCodepath
    {
        SetWithLatestData = 0b110,
        NoSetWithOutdatedData = 0b001, // and 101
        NoSetTimeoutLatest = 0b010,
        NoSetTimeoutOtherLatest = 0b000,
        NoSetDelayOtherLatest = 0b100,
        UpdatedDeadline = 0b1000,
    };

#ifdef WAIT_CODEPATHS
private:
    std::unordered_set<WaitCodepath> m_wait_codepaths{};

public:
    std::unordered_set<WaitCodepath> const& _wait_codepaths() const
    {
        return m_wait_codepaths;
    }

#define wait_codepath(path) m_wait_codepaths.insert(path)
#else
#define wait_codepath(path)
#endif // WAIT_CODEPATHS

private:
    WaitResult wait(std::unique_lock<std::mutex>& lock)
    {
        clock::time_point call_time{ clock::now() };
        clock::time_point deadline{};
        bool ok{ false };

        m_set_latest = std::max(m_set_latest, call_time);

        while (true) {
            deadline = m_set_deadline;
            ok = m_set_cv.wait_until(lock, deadline, [this, call_time] {
                return m_refs == 0 && call_time == m_set_latest ||
                    m_set_latest == clock::time_point::min();
            });
            if (!ok && deadline < m_set_deadline) {
                // The condition is not satisfied and the deadline was updated.
                // Since there is more time, simply iterate and wait longer.
                wait_codepath(WaitCodepath::UpdatedDeadline);
                continue;
            }

            // Handle all possible cases in which wait_until() could return.
            // In all these cases the deadline was exceeded and has no updates.
            // All cases are ordered by their likelihood.

            auto a = m_refs == 0;
            auto b = call_time == m_set_latest;
            auto c = m_set_latest == clock::time_point::min();

            if (a && b && !c) { // 110: ok
                wait_codepath(WaitCodepath::SetWithLatestData);
                // Data can be set and this set call contains the latest data.
                assert(ok);
                break;

            } else if (!b && c) { // 001 / 101: ok
                wait_codepath(WaitCodepath::NoSetWithOutdatedData);
                // Another more recent set call has made its changes,
                // therefore we can return immediately.
                assert(ok);
                return WaitResult::Outdated;

            } else if (!a && b && !c) { // 010: timeout
                wait_codepath(WaitCodepath::NoSetTimeoutLatest);
                // Data cannot be set, but this is the latest set call.
                // It has timed out and an exception needs to be thrown.
                assert(!ok);
                break;

            } else if (!a && !b && !c) { // 000: timeout
                wait_codepath(WaitCodepath::NoSetTimeoutOtherLatest);
                // This set call has timed out and data cannot be set.
                // Additionally, there is another set call that is more recent
                // and whose data should be set instead, but since the deadline
                // has not been updated, the other set call will throw.
                // In that case this set call should throw as well.
                assert(!ok);
                assert(deadline == m_set_deadline);
                break;

            } else if (a && !b && !c) { // 100: timeout
                wait_codepath(WaitCodepath::NoSetDelayOtherLatest);
                // Data can be set, but there is another set call that
                // is more recent and whose data should be set instead.
                // Additionally this set call has timed out
                // and there is no update to the deadline.
                assert(!ok);
                assert(m_set_latest > call_time);
                assert(deadline == m_set_deadline);

                // This case is extremely unlikely to happen in practice,
                // since both the reference count has reached zero and the
                // deadline has been reached at exactly the same time, yet
                // the latest set call has not modified the value yet.
                // Simply sleep for a small duration to give the latest set
                // call time to make its changes, then continue the loop so
                // that this set call can return after the new value has
                // been set. We cannot return now, as set must only return
                // after a value update.
                std::this_thread::sleep_for(100ns);
                continue;

            } else if (!a && b && c) { // 011 / 111
                // This cannot happen: b and c are mutually exclusive.
                assert(call_time != clock::time_point::min());
            }

            assert(false);
            throw std::runtime_error("impossible state");
        }

        return ok ? WaitResult::Ok : WaitResult::Timeout;
    }

#undef wait_codepath

    /**
     * @brief Handler for the destruction of values returned by Atomic::get.
     *
     * Decrements the reference count and notifies any Atomic::set calls.
     */
    void get_dtor()
    {
        // Since this method may be called often when the return value of get()
        // is treated as a temporary r-value rather than being stored somewhere,
        // it is a good idea to optimize it a little bit by not locking a mutex
        // everytime the shared pointer's destructor is called.

        auto old_refs = decr_refs();
        if (old_refs <= 1) {
            assert(old_refs != 0);
            m_set_cv.notify_all();
        }
    }

    /**
     * @brief Decrements the reference counter by 1 if it's greater than 0.
     *
     * This method is guaranteed to never underflow (decrement below zero).
     * When this method returns zero, the reference count was not modified.
     *
     * @returns The old value of the reference counter.
     */
    size_t decr_refs()
    {
        auto refs = m_refs.load();
        while (true) {
            if (refs == 0) {
                break;
            }
            auto new_refs = refs - 1;
            if (m_refs.compare_exchange_weak(refs, new_refs)) {
                break;
            }
        }
        return refs;
    }

    std::mutex m_mutex{};
    std::condition_variable m_set_cv{};
    std::atomic<size_t> m_refs{ 0 };
    clock::time_point m_set_deadline{ clock::time_point::min() };
    clock::time_point m_set_latest{ clock::time_point::min() };
    std::function<void(T const&)> m_callback{ nullptr };

    // A pointer to the stored value. The pointer is never modified or replaced.
    // The value pointed to by the pointer may be be modified though.
    const std::shared_ptr<T> m_value{};

    // Holds whether this Atomic instance is active and the destructor
    // for references returned by Atomic::get is safe to be called.
    const std::shared_ptr<std::atomic<bool>> m_active{
        std::make_shared<std::atomic<bool>>(true)
    };

#ifdef UNGIVE_UTILITY_TEST
public:
    // Returns a copy of the internally stored value.
    inline std::shared_ptr<const T> _value() const { return m_value; }
#endif // UNGIVE_UTILITY_TEST

#ifdef TRACK_LIFETIMES
public:
    inline void _stop_lifetime_tracking() { stop_lifetime_thread(); }

#ifdef LIFETIME_RECORDING
private:
    bool m_lifetime_record_history{ false };
    std::condition_variable m_lifetime_history_cv{};
    std::deque<std::pair<std::chrono::milliseconds, bool>> m_lifetime_history;

public:
    // Records lifetime errors instead of causing assertion errors.
    void _record_lifetime_history()
    {
        std::lock_guard<std::mutex> lock(m_lifetime_mutex);
        m_lifetime_record_history = true;
    }

    // Pops the last lifetime error in the queue.
    inline decltype(m_lifetime_history)::value_type
    _await_lifetime_history_entry()
    {
        std::unique_lock<std::mutex> lock(m_lifetime_mutex);
        m_lifetime_history_cv.wait_for(lock, std::chrono::seconds{ 1 }, [this] {
            return !m_lifetime_history.empty();
        });
        assert(!m_lifetime_history.empty());
        auto front = m_lifetime_history.front();
        m_lifetime_history.pop_front();
        return front;
    }
#endif // LIFETIME_RECORDING

private:
    std::thread m_lifetime_thread{};
    std::mutex m_lifetime_mutex{};
    std::condition_variable m_lifetime_cv{};
    std::multiset<clock::time_point> m_lifetime_expirations{};
    bool m_lifetime_update{ false };
    bool m_lifetime_stop{ false };

    void track_lifetimes()
    {
        std::unique_lock<std::mutex> lock(m_lifetime_mutex);
        while (!m_lifetime_stop) {
            auto timepoint = clock::time_point::max();
            if (!m_lifetime_expirations.empty()) {
                timepoint = *m_lifetime_expirations.begin();
            }

#ifdef LIFETIME_RECORDING
            auto start = clock::now();
#endif // LIFETIME_RECORDING

            m_lifetime_cv.wait_until(lock, timepoint, [this] {
                return m_lifetime_update || m_lifetime_stop;
            });
            if (m_lifetime_stop) {
                break;
            }
            if (m_lifetime_update) {
                m_lifetime_update = false;
#ifdef LIFETIME_RECORDING
                if (m_lifetime_record_history) {
                    m_lifetime_history.push_back(std::make_pair(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            clock::now() - start),
                        false));
                    m_lifetime_history_cv.notify_all();
                }
#endif // LIFETIME_RECORDING
                continue;
            }

            assert(!m_lifetime_expirations.empty());
            assert(timepoint == *m_lifetime_expirations.begin());
            assert(clock::now() >= timepoint);

            m_lifetime_expirations.erase(m_lifetime_expirations.begin());

            bool assertion_error = true;
#ifdef LIFETIME_RECORDING
            if (m_lifetime_record_history) {
                m_lifetime_history.push_back(std::make_pair(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock::now() - start),
                    true));
                m_lifetime_history_cv.notify_all();
                assertion_error = false;
            }
#endif // LIFETIME_RECORDING

            if (assertion_error) {
                // A get key is being used beyond its lifetime.
                assert(false && "a reference is used beyond its lifetime");
            }
        }
    }

    void get_dtor_tracking(clock::time_point timepoint)
    {
        {
            std::lock_guard<std::mutex> lock(m_lifetime_mutex);
            auto it = m_lifetime_expirations.find(timepoint);
            if (it != m_lifetime_expirations.end()) {
                // Only erase one item with this value, not all.
                m_lifetime_expirations.erase(it);
            }
            m_lifetime_update = true;
        }
        get_dtor();
    }

    void stop_lifetime_thread()
    {
        {
            std::lock_guard<std::mutex> lock(m_lifetime_mutex);
            if (m_lifetime_stop) {
                return;
            }
            m_lifetime_stop = true;
            m_lifetime_cv.notify_all();
        }
        m_lifetime_thread.join();
    }
#endif // TRACK_LIFETIMES
};

} // namespace utility
} // namespace ungive

#ifndef UNGIVE_UTILITY_TEST
#ifdef TRACK_LIFETIMES
#undef TRACK_LIFETIMES
#endif
#ifdef LIFETIME_RECORDING
#undef LIFETIME_RECORDING
#endif
#ifdef WAIT_CODEPATHS
#undef WAIT_CODEPATHS
#endif
#endif // UNGIVE_UTILITY_TEST
