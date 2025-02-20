#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

// Track get() lifetimes during debugging.
#if !defined(NDEBUG) && !defined(NO_TRACK_LIFETIMES)
#define TRACK_LIFETIMES
#include <set>
#endif

// Enable lifetime recording during unit tests.
#if defined(TRACK_LIFETIMES) && defined(UNGIVE_UTILITY_TEST)
#define LIFETIME_RECORDING
#include <deque>
#endif

namespace ungive::utility
{

/**
 * @brief Wraps a value that can be updated safely from other threads.
 *
 * Values can be retrieved by reference (without copying)
 * and can be updated whenever there is no reference to it in active use.
 * Whenver a reference to the value is retrieved via get() any calls to set()
 * block until all returned references from get() have been destructed.
 *
 * @tparam T The value type.
 */
template <typename T>
class LiveValue
{
private:
    LiveValue(std::shared_ptr<T>&& ptr) : m_value{ std::move(ptr) }
    {
#ifdef TRACK_LIFETIMES
        m_lifetime_thread = std::thread(&LiveValue::track_lifetimes, this);
#endif
    }

public:
    using value_type = T;

    // The default lifetime for returned get values.
    static constexpr auto default_get_lifetime =
        std::chrono::milliseconds{ 100 };

    LiveValue() : LiveValue(std::make_shared<T>()) {}

    LiveValue(T&& value) : LiveValue(std::make_shared<T>(std::move(value))) {}

    LiveValue(T const& value) : LiveValue(std::make_shared<T>(value)) {}

    template <typename... Args>
    LiveValue(Args&&... args)
        : LiveValue(std::make_shared<T>(std::forward<Args>(args)...))
    {
    }

    ~LiveValue()
    {
        // TODO wrap all of this in a shared_ptr and pass the shared_ptr
        // to each called that calls get(), so that there's a reference to it
        // as long as a returned value from get() exists.
        // that way we don't have to worry about deleted memory possibly
        // being used somewhere in the program.

        const std::lock_guard lock(m_mutex);
        assert(m_refs == 0 && "dangling get references exist");

#ifdef TRACK_LIFETIMES
        stop_lifetime_thread();
#ifdef LIFETIME_RECORDING
        // There should be no errors left in the history.
        assert(m_lifetime_history.empty());
#endif // LIFETIME_RECORDING
#endif // TRACK_LIFETIMES
    }

    /**
     * @brief Returns a value reference and increments the reference count.
     *
     * The reference count is decremented when the returned value is destructed.
     * It is recommended to not store the return value for a long time
     * and instead only treat it as a temporary r-value.
     * Storing it for a long time is considered bad pratice
     * and may cause substantial delays when calling set().
     *
     * The returned value is expected to be destructed after
     * the amount of time in the passed lifetime has elapsed.
     * If it is used for longer, calls to set() may throw.
     * The passed lifetime must be greater than zero.
     *
     * The return value must not be stored beyond the lifetime
     * of this class instance, otherwise deleted memory may be used.
     *
     * @param lifetime How long the returned value will be used at most.
     *
     * @returns A reference-counted pointer to the internally stored value.
     */
    std::shared_ptr<T> get(
        std::chrono::milliseconds lifetime = LiveValue::default_get_lifetime)
    {
        if (lifetime <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "the lifetime must be greater than zero");
        }

        const std::lock_guard lock(m_mutex);
        m_refs += 1;
        auto timepoint = std::chrono::steady_clock::now() + lifetime;

#ifdef TRACK_LIFETIMES
        auto destructor =
            std::bind(&LiveValue<T>::get_dtor_tracking, this, timepoint);
        {
            const std::lock_guard lock(m_lifetime_mutex);
            m_lifetime_expirations.insert(timepoint);
            m_lifetime_update = true;
            m_lifetime_cv.notify_all();
        }
#else
        auto destructor = std::bind(&LiveValue<T>::get_dtor, this);
#endif // TRACK_LIFETIMES

        std::shared_ptr<T> result(m_value.get(), destructor);
        m_set_wait = std::max(m_set_wait, timepoint);
        return result;
    }

    /**
     * @brief Atomically sets the internal value to the given value.
     *
     * Blocks until all references returned from get() are destroyed
     * or the operation times out after the given timeout duration.
     *
     * The timeout must be larger than zero.
     * The default timeout is sufficiently large to account for references
     * from get() to be stored temporarily with possibly somewhat long-running
     * operations, but small enough for the operation to not block forever.
     *
     * @param value The value to set.
     *
     * @throws std::runtime_error when a reference returned from get()
     * is used beyond it's promised lifetime.
     */
    void set(T&& value)
    {
        std::unique_lock lock(m_mutex);
        decltype(m_set_wait) timepoint{};
        bool ok{ false };

        // Loop in case the timepoint has been updated by another get() call.
        do {
            timepoint = m_set_wait;
            ok = m_cv.wait_until(lock, timepoint, [this] {
                return m_refs == 0;
            });
        } while (!ok && timepoint < m_set_wait);

        if (!ok) {
            throw std::runtime_error("a reference is used beyond its lifetime");
        }
        *m_value = std::move(value);
    }

    /**
     * @brief Atomically sets the internal value to the given value.
     *
     * @see LiveValue::set
     *
     * @param value The value to set.
     */
    template <typename... Args>
    inline void set(Args&&... args)
    {
        set(T(std::forward<Args>(args)...));
    }

private:
    /**
     * @brief Handler for the destruction of values returned by get().
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
            m_cv.notify_all();
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
    std::condition_variable m_cv{};
    std::atomic<size_t> m_refs{ 0 };
    std::chrono::steady_clock::time_point m_set_wait{
        std::chrono::steady_clock::time_point::min()
    };

    // A pointer to the stored value. The pointer is never modified or replaced.
    // The value pointed to by the pointer may be be modified though.
    const std::shared_ptr<T> m_value{};

#ifdef TRACK_LIFETIMES

#ifdef LIFETIME_RECORDING
private:
    bool m_lifetime_record_history{ false };
    std::condition_variable m_lifetime_history_cv{};
    std::deque<std::pair<std::chrono::milliseconds, bool>> m_lifetime_history;

public:
    inline void _stop_lifetime_tracking() { stop_lifetime_thread(); }

    // Records lifetime errors instead of causing assertion errors.
    void _record_lifetime_history()
    {
        const std::lock_guard lock(m_lifetime_mutex);
        m_lifetime_record_history = true;
    }

    // Pops the last lifetime error in the queue.
    inline decltype(m_lifetime_history)::value_type
    _await_lifetime_history_entry()
    {
        std::unique_lock lock(m_lifetime_mutex);
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
    std::multiset<std::chrono::steady_clock::time_point>
        m_lifetime_expirations{};
    bool m_lifetime_update{ false };
    bool m_lifetime_stop{ false };

    void track_lifetimes()
    {
        std::unique_lock lock(m_lifetime_mutex);
        while (!m_lifetime_stop) {
            auto timepoint = std::chrono::steady_clock::time_point::max();
            if (!m_lifetime_expirations.empty()) {
                timepoint = *m_lifetime_expirations.begin();
            }

#ifdef LIFETIME_RECORDING
            auto start = std::chrono::steady_clock::now();
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
                            std::chrono::steady_clock::now() - start),
                        false));
                    m_lifetime_history_cv.notify_all();
                }
#endif // LIFETIME_RECORDING
                continue;
            }

            assert(!m_lifetime_expirations.empty());
            assert(timepoint == *m_lifetime_expirations.begin());
            assert(std::chrono::steady_clock::now() >= timepoint);

            m_lifetime_expirations.erase(m_lifetime_expirations.begin());

            bool assertion_error = true;
#ifdef LIFETIME_RECORDING
            if (m_lifetime_record_history) {
                m_lifetime_history.push_back(std::make_pair(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start),
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

    void get_dtor_tracking(std::chrono::steady_clock::time_point timepoint)
    {
        {
            const std::lock_guard lock(m_lifetime_mutex);
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
            const std::lock_guard lock(m_lifetime_mutex);
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

} // namespace ungive::utility

#ifdef TRACK_LIFETIMES
#undef TRACK_LIFETIMES
#endif

#ifdef LIFETIME_RECORDING
#undef LIFETIME_RECORDING
#endif
