#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

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
public:
    using value_type = T;

    // The default lifetime for returned get values.
    static constexpr auto default_get_lifetime =
        std::chrono::milliseconds{ 100 };

    LiveValue() : m_value{ std::make_shared<T>() } {}

    LiveValue(T&& value) : m_value{ std::make_shared<T>(std::move(value)) } {}

    LiveValue(T const& value) : m_value{ std::make_shared<T>(value) } {}

    template <typename... Args>
    LiveValue(Args&&... args)
        : m_value{ std::make_shared<T>(std::forward<Args>(args)...) }
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
        // TODO during debugging/development:
        // track that each returned value is destructed before it lifetime ends.
        // spin up another thread that checks this. that should obviously
        // not be done during production (too much overhead).

        if (lifetime <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "the lifetime must be greater than zero");
        }

        const std::lock_guard lock(m_mutex);
        m_refs += 1;
        std::shared_ptr<T> result(
            m_value.get(), std::bind(&LiveValue<T>::get_dtor, this));
        m_set_wait =
            std::max(m_set_wait, std::chrono::steady_clock::now() + lifetime);
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
    std::condition_variable m_cv;
    std::atomic<size_t> m_refs{ 0 };
    std::chrono::steady_clock::time_point m_set_wait{
        std::chrono::steady_clock::time_point::min()
    };

    // A pointer to the stored value. The pointer is never modified or replaced.
    // The value pointed to by the pointer may be be modified though.
    const std::shared_ptr<T> m_value{};
};

} // namespace ungive::utility
