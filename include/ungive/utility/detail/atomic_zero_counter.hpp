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
template <typename V>
class AtomicZeroCounter
{
private:
    using self_type = AtomicZeroCounter<V>;

    static_assert(
        std::is_integral<V>::value, "the value type must be integral");
    static_assert(std::is_signed<V>::value, "the value type must be signed");

    static constexpr V min_value = std::numeric_limits<V>::min();
    static constexpr V max_value = std::numeric_limits<V>::max();

    static_assert(max_value > 0 && min_value < 0 && -max_value >= min_value,
        "every positive value must be representable with a negative sign");

    static constexpr V zero_delta = V(1);
    static constexpr V positive_one = V(+1);
    static constexpr V negative_one = V(-1);

    // Returns the real counter value of the stored value.
    static inline V real(V value)
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
    // The type of the counter value.
    using value_type = V;

    // Return value of AtomicZeroCounter::incr when the operation failed.
    static constexpr V fail = -1;

    /**
     * @brief Loads the current counter value.
     *
     * @returns The currently stored counter value.
     */
    V load() const { return real(m_value.load()); }

    /**
     * @brief Attempts to increment the counter by 1.
     *
     * Returns the old counter value, if the operation succeeded.
     * The operation may fail when another thread has called
     * the AtomicZeroCounter::stop method to stop incrementation.
     * In that case AtomicZeroCounter::fail is returned to indicate failure.
     *
     * @returns The old value or AtomicZeroCounter::fail in case of failure.
     */
    V incr()
    {
        auto value = m_value.load();
        while (true) {
            auto sign = sgn(value);
            if (sign == -1) {
                return self_type::fail;
            }
            if (sign == 0) {
                assert(false);
                return self_type::fail;
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
    V decr()
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
    std::atomic<V> m_value{ positive_one };
    std::promise<bool> m_promise{};
};

/**
 * @brief Increments a counter and decrements when it goes out of scope.
 *
 * The counter is incremented upon instantiation and decremented when the
 * counter_guard instance goes out of scope, unless the increment operation
 * failed, in which case no decrement operation is performed.
 *
 * The counter type C must have a member method incr() that returns the value
 * of the counter type's static data member "fail" when the increment operation
 * fails and no decrement operation should follow. The counter type must also
 * have a decr() method that returns the old value of the counter before the
 * decrement operation was performed. decr() must always return a value
 * greater than zero, when called after a successful call to incr().
 *
 * @tparam C The counter type.
 */
template <typename C>
class counter_guard
{
private:
    // Return type of the counter's incr() method.
    using R = decltype(C::fail);

    // Return value of incr() that indicates operation failure.
    static constexpr R incr_fail = C::fail;

    static_assert(std::is_same<std::decay<R>::type,
                      std::decay<decltype(((C*)nullptr)->incr())>::type>::value,
        "the fail value type and the incr return type must be identical");

public:
    /**
     * @brief Creates a new counter guard and attempts to increment the counter.
     *
     * If the increment operation fails, the counter guard evaluates to true.
     *
     * @param counter
     */
    counter_guard(C& counter)
        : m_counter{ counter }, m_incr_result{ m_counter.incr() }
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
    inline bool was_incremented() const { return m_incr_result != incr_fail; }

    C& m_counter;
    R m_incr_result;
};

} // namespace detail
} // namespace utility
} // namespace ungive
