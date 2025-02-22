// Copyright (c) 2025 Jonas van den Berg
// All rights reserved.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ungive
{
namespace utility
{
namespace detail
{

/**
 * @brief Returns the sign of a value.
 *
 * +1 is returned for positive values.
 * -1 is returned for negative values.
 * 0 is returned for zero.
 *
 * @param value The value to return the sign for.
 * @returns An integer indicating the sign of the value.
 */
template <typename T>
static constexpr int sgn(T value)
{
    return (T(0) < value) - (value < T(0));
}

/**
 * @brief A counter that prevents incrementation and can notify when it hits 0.
 *
 * This is a counter implementation with increment and decrement operations.
 * It offers a AtomicZeroCounter::stop method that prohibits any further
 * increments and blocks until the counter has been decremented back to zero.
 */
struct AtomicZeroCounter
{
    using value_type = intmax_t;

private:
    static constexpr value_type max_value =
        std::numeric_limits<value_type>::max();

    static constexpr value_type zero_delta = 1;
    static constexpr value_type positive_one = +1;
    static constexpr value_type negative_one = -1;

    // Returns the real counter value of the stored value.
    static inline value_type real(value_type value)
    {
        if (value == 0) {
            assert(false);
            return 0;
        }
        return std::abs(value) - zero_delta;
    }

    static_assert(sgn(0) == 0, "");
    static_assert(sgn(positive_one) == positive_one, "");
    static_assert(sgn(negative_one) == negative_one, "");
    static_assert(positive_one - zero_delta == 0, "");
    static_assert(negative_one + zero_delta == 0, "");

public:
    /**
     * @brief Loads the current counter value.
     *
     * @returns The currently stored counter value.
     */
    value_type load() const { return real(m_value.load()); }

    /**
     * @brief Attempts to increment the counter by 1.
     *
     * Returns the old counter value, if the operation succeeded.
     * The operation may fail when another thread has called
     * the AtomicZeroCounter::stop method to stop incrementation.
     * In that case -1 is returned to indicate failure.
     *
     * @returns The old value or -1, if the operation failed.
     */
    value_type incr()
    {
        auto value = m_value.load();
        while (true) {
            auto sign = sgn(value);
            if (sign == -1) {
                // A negative sign indicates to not increment anymore.
                return -1;
            }
            if (sign == 0) {
                assert(false);
                return -1;
            }
            assert(sign == +1);
            assert(value != max_value);
            auto new_value = value + 1;
            // The incr() method should never change the sign.
            assert(sgn(value) == sgn(new_value));
            if (m_value.compare_exchange_weak(value, new_value)) {
                break;
            }
        }
        assert(sgn(value) != -1);
        return real(value);
    }

    /**
     * @brief Decrements the counter by 1, if it is greater than 0.
     *
     * This method is guaranteed to never underflow (decrement below zero).
     * When this method returns zero, the counter value was not modified.
     *
     * @returns The old value of the counter before decrementing.
     */
    value_type decr()
    {
        auto value = m_value.load();
        while (true) {
            if (value == positive_one || value == negative_one) {
                // We already decremented to the lowest possible value.
                assert(real(value) == 0);
                break;
            }
            auto sign = sgn(value);
            if (sign == 0) {
                assert(false);
                return 0;
            }
            auto new_value = value - sign;
            assert(std::abs(new_value) == std::abs(value) - 1);
            // The decr() method should never change the sign.
            assert(sgn(value) == sgn(new_value));
            if (m_value.compare_exchange_weak(value, new_value)) {
                if (sign == -1 && new_value == negative_one) {
                    assert(real(new_value) == 0);
                    // Notify the promise that zero has been reached.
                    m_promise.set_value(true);
                }
                break;
            }
        }
        return real(value);
    }

    /**
     * @brief Waits until zero is reached and prevents incrementation.
     *
     * This method prevents further calls to AtomicZeroCounter::incr
     * from incrementing the counter. AtomicZeroCounter::decr calls
     * are not affected and can continue to decrement.
     *
     * The method blocks until the counter has reached a value of 0.
     * Upon returning the counter is guaranteed to be and stay at 0.
     *
     * This method may only be called once and never again.
     */
    void stop()
    {
        // Invert the sign of the currently stored value.
        // This tells the decr() method to notify when zero is reached.
        auto value = m_value.load();
        while (true) {
            auto sign = sgn(value);
            if (sign == -1) {
                // stop() can only be called once.
                throw std::runtime_error("stop was already called");
            }
            if (sign == 0) {
                assert(false);
                return;
            }
            // A negative integer can store every positive integer whose sign
            // is flipped to make it negative, since the absolute value of the
            // smallest possible negative integer value is one larger than the
            // largest possible value of the same integer type.
            auto new_value = -value;
            assert(value != new_value);
            assert(sgn(new_value) == -1);
            if (m_value.compare_exchange_weak(value, new_value)) {
                break;
            }
        }

        // Wait for the value to reach zero, if it hasn't already.
        if (real(value) != 0) {
            auto future = m_promise.get_future();
            assert(future.valid());
            auto result = future.get();
            assert(result);
        }

        assert(real(m_value.load()) == 0);
        assert(m_value.load() != 0);
    }

private:
    // A single atomic guarantees thread-safety and prevents data races.
    std::atomic<value_type> m_value{ positive_one };
    std::promise<bool> m_promise{};
};

/**
 * @brief Increments a counter and decrements when it goes out of scope.
 *
 * The counter must have an incr() method that returns a specific value
 * (specified as NotIncrementedResult) when the increment operation fails.
 * The counter must have a decr() method that returns the old value that was
 * stored in the counter before the decrement operation was performed.
 * decr() should always return a value greater than zero, when called
 * after a successful call to incr().
 *
 * @tparam C The counter type.
 * @tparam R The result type of the incr() method.
 * @tparam NotIncrementedResult The return value of failed incr() calls.
 */
template <typename C,
    typename R = std::conditional_t<std::is_same_v<C, AtomicZeroCounter>,
        AtomicZeroCounter::value_type, bool>,
    R NotIncrementedResult = std::is_same_v<C, AtomicZeroCounter> ? -1 : false>
class counter_guard
{
public:
    /**
     * @brief Creates a new counter guard and attempts to increment the counter.
     *
     * If the increment operation fails, the counter guard evaluates to true.
     *
     * @param counter
     */
    counter_guard(C& counter)
        : m_counter(counter), m_incr_result{ m_counter.incr() }
    {
    }

    ~counter_guard()
    {
        if (was_incremented()) {
            auto old_value = m_counter.decr();
            assert(old_value > 0);
        }
    }

    /**
     * @brief Returns the result of the incr() call of the counter.
     *
     * @returns The result of the increment operation.
     */
    R result() const { return m_incr_result; }

    /**
     * @brief Returns whether the counter was incremented upon instantiation.
     *
     * @returns Whether the counter was incremented.
     */
    operator bool() const { return was_incremented(); }

    counter_guard(const counter_guard&) = delete;
    counter_guard& operator=(const counter_guard&) = delete;

private:
    inline bool was_incremented() const
    {
        return m_incr_result != NotIncrementedResult;
    }

    C& m_counter;
    R m_incr_result;
};

} // namespace detail
} // namespace utility
} // namespace ungive
