#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ungive/utility/atomic.hpp"

using namespace testing;
using namespace ungive::utility::detail;
using namespace std::chrono_literals;

TEST(AtomicZeroCounter, StopBlocksUntilCounterIsDecrementedToZero)
{
    constexpr auto sleep_duration = 152ms;
    AtomicZeroCounter c;
    c.incr();
    std::mutex mutex;
    std::condition_variable cv;
    auto after_decr = std::chrono::steady_clock::time_point::min();
    std::thread t1([&] {
        std::this_thread::sleep_for(sleep_duration);
        c.decr();
        {
            std::lock_guard<std::mutex> lock(mutex);
            after_decr = std::chrono::steady_clock::now();
            cv.notify_all();
        }
    });
    auto before_stop = std::chrono::steady_clock::now();
    c.stop();
    auto after_stop = std::chrono::steady_clock::now();
    EXPECT_GE(after_stop - before_stop, sleep_duration - 25ms);
    EXPECT_LE(after_stop - before_stop, sleep_duration + 25ms);
    {
        std::unique_lock<std::mutex> lock(mutex);
        auto ok = cv.wait_for(lock, 1s, [&] {
            return after_decr != std::chrono::steady_clock::time_point::min();
        });
        ASSERT_TRUE(ok);
    }
    EXPECT_GE(after_decr - after_stop, -5ms);
    EXPECT_LE(after_decr - after_stop, 5ms);
    t1.join();
}

TEST(AtomicZeroCounter, StopReturnsImmediatelyWhenCounterIsAtZero)
{
    AtomicZeroCounter c;
    auto start = std::chrono::steady_clock::now();
    c.stop();
    auto delta = std::chrono::steady_clock::now() - start;
    EXPECT_LT(delta, 1ms);
}

TEST(AtomicZeroCounter, IncrementingAfterStopFails)
{
    AtomicZeroCounter c;
    c.stop();
    EXPECT_EQ(-1, c.incr());
    EXPECT_EQ(0, c.load());
}

TEST(AtomicZeroCounter, IncrReturnsOldValue)
{
    AtomicZeroCounter c;
    EXPECT_EQ(0, c.incr());
    EXPECT_EQ(1, c.incr());
    c.decr();
    EXPECT_EQ(1, c.incr());
    EXPECT_EQ(2, c.incr());
}

TEST(AtomicZeroCounter, DecrAfterStopReturnsPositiveValue)
{
    AtomicZeroCounter c;
    c.incr();
    std::thread t1([&] {
        c.stop();
    });
    std::this_thread::sleep_for(25ms);
    EXPECT_EQ(1, c.decr());
    t1.join();
}

TEST(AtomicZeroCounter, LoadReturnsCurrentValue)
{
    AtomicZeroCounter c;
    EXPECT_EQ(0, c.load());
    c.incr();
    EXPECT_EQ(1, c.load());
    c.incr();
    EXPECT_EQ(2, c.load());
    c.decr();
    EXPECT_EQ(1, c.load());
    c.incr();
    EXPECT_EQ(2, c.load());
    c.incr();
    EXPECT_EQ(3, c.load());
}

TEST(counter_guard, EvaluatesToTrueWhenIncrementSucceeded)
{
    AtomicZeroCounter c;
    {
        counter_guard<AtomicZeroCounter> guard(c);
        EXPECT_TRUE(guard);
        EXPECT_EQ(1, c.load());
    }
}

TEST(counter_guard, EvaluatesToFalseWhenIncrementFailed)
{
    AtomicZeroCounter c;
    c.stop();
    {
        counter_guard<AtomicZeroCounter> guard(c);
        EXPECT_FALSE(guard);
        EXPECT_EQ(0, c.load());
    }
}

TEST(counter_guard, DoesNotDecrementWhenIncrementFailed)
{
    AtomicZeroCounter c;
    c.incr();
    std::promise<bool> promise;
    std::thread t1([&] {
        promise.set_value(true);
        c.stop();
    });
    promise.get_future().get();
    std::this_thread::sleep_for(5ms);
    {
        counter_guard<AtomicZeroCounter> guard(c);
        EXPECT_FALSE(guard);
        EXPECT_EQ(1, c.load());
    }
    c.decr();
    t1.join();
}

TEST(counter_guard, DecrementsAfterDestruction)
{
    AtomicZeroCounter c;
    EXPECT_EQ(0, c.load());
    {
        counter_guard<AtomicZeroCounter> g1(c);
        EXPECT_TRUE(g1);
        EXPECT_EQ(1, c.load());
        {
            counter_guard<AtomicZeroCounter> g2(c);
            EXPECT_TRUE(g2);
            EXPECT_EQ(2, c.load());
        }
        EXPECT_EQ(1, c.load());
    }
    EXPECT_EQ(0, c.load());
}

TEST(counter_guard, ResultReturnsTheIncrementReturnValue)
{
    {
        AtomicZeroCounter c1;
        AtomicZeroCounter c2;
        {
            counter_guard<AtomicZeroCounter> g1(c1);
            EXPECT_EQ(c2.incr(), g1.result());
            {
                counter_guard<AtomicZeroCounter> g2(c1);
                EXPECT_EQ(c2.incr(), g2.result());
            }
            c2.decr();
        }
        c2.decr();
        counter_guard<AtomicZeroCounter> g3(c1);
        EXPECT_EQ(c2.incr(), g3.result());
    }
    {
        AtomicZeroCounter c1;
        AtomicZeroCounter c2;
        {
            counter_guard<AtomicZeroCounter> g1(c1);
            EXPECT_EQ(c2.incr(), g1.result());
        }
        c1.decr();
        c2.decr();
        c1.stop();
        c2.stop();
        counter_guard<AtomicZeroCounter> g2(c1);
        EXPECT_EQ(c2.incr(), g2.result());
        counter_guard<AtomicZeroCounter> g3(c1);
        EXPECT_EQ(c2.incr(), g3.result());
        EXPECT_EQ(-1, g3.result());
    }
}
