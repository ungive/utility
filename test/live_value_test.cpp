#include <chrono>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ungive/utility/live_value.h"

using namespace ungive::utility;
using namespace std::chrono_literals;

static const int TEST_VALUE_DEFAULT_X = 34;

struct TestValue
{
    TestValue() {}

    TestValue(int value) : x{ value } {}

    int x{ TEST_VALUE_DEFAULT_X };
};

struct TestValueWithCtor
{
    TestValueWithCtor() = delete;

    TestValueWithCtor(int value) : x{ value } {}

    int x{ TEST_VALUE_DEFAULT_X };
};

TEST(LiveValue, ConstructWithDefaultConstructor)
{
    LiveValue<TestValue> c;
    EXPECT_EQ(TEST_VALUE_DEFAULT_X, c.get()->x);
}

TEST(LiveValue, ConstructWithMoveConstructor)
{
    int expected = 102;
    TestValue value{ expected };
    LiveValue<TestValue> c(std::move(value));
    EXPECT_EQ(expected, c.get()->x);
}

TEST(LiveValue, ConstructWithCopyConstructor)
{
    TestValue value{ 103 };
    LiveValue<TestValue> c(value);
    EXPECT_EQ(value.x, c.get()->x);
}

TEST(LiveValue, ConstructWithInitConstructor)
{
    int expected = 104;
    LiveValue<TestValue> c(104);
    EXPECT_EQ(expected, c.get()->x);
}

TEST(LiveValue, SetBlocksUntilGetReturnValueIsDestructed)
{
    LiveValue<TestValue> c(1);
    std::thread t1([&] {
        auto ref = c.get(125ms);
        ref->x = 2;
        // Pretend it takes this long to update the value.
        std::this_thread::sleep_for(100ms);
    });
    std::thread t2([&] {
        std::this_thread::sleep_for(25ms);
        auto start = std::chrono::steady_clock::now();
        // Attempt to set the value mid-updating of the other thread.
        // This should be blocking until the other thread is done.
        c.set({ 3 });
        auto delta = std::chrono::steady_clock::now() - start;
        EXPECT_GT(delta, 50ms);
        EXPECT_EQ(3, c.get()->x);
    });
    t1.join();
    t2.join();
    EXPECT_EQ(3, c.get()->x);
}

TEST(LiveValue, CopyOfGetReturnValueMakesSetBlockAsWell)
{
    LiveValue<TestValue> c(1);
    std::thread t1([&] {
        std::shared_ptr<TestValue> copied_ref{ nullptr };
        {
            auto ref = c.get(125ms);
            ref->x = 2;
            copied_ref = ref;
        }
        copied_ref->x = 3;
        std::this_thread::sleep_for(100ms);
    });
    std::thread t2([&] {
        std::this_thread::sleep_for(25ms);
        auto start = std::chrono::steady_clock::now();
        c.set({ 4 });
        auto delta = std::chrono::steady_clock::now() - start;
        EXPECT_GT(delta, 50ms);
        EXPECT_EQ(4, c.get()->x);
    });
    t1.join();
    t2.join();
    EXPECT_EQ(4, c.get()->x);
}

TEST(LiveValue, DiesWhenLiveValueIsDestructedBeforeGetReturnValue)
{
    std::shared_ptr<TestValue> dangling_ref{ nullptr };
    EXPECT_DEATH(
        {
            LiveValue<TestValue> c(1);
            dangling_ref = c.get();
        },
        "");
}

TEST(LiveValue, SetTimesOutWhenGetReturnValueLivesBeyondItsLifetime)
{
    LiveValue<TestValue> c(1);
    auto ref = c.get(100ms);
    auto start = std::chrono::steady_clock::now();
    EXPECT_ANY_THROW(c.set({ 2 }));
    auto delta = std::chrono::steady_clock::now() - start;
    EXPECT_GT(delta, 75ms);
}

TEST(LiveValue, GetReturnsChangedValueAfterUpdatingValueWithSet)
{
    LiveValue<TestValue> c(1);
    c.set({ 2 });
    EXPECT_EQ(2, c.get()->x);
}

TEST(LiveValue, SetDoesNotBlockWhenGetWasNeverCalled)
{
    LiveValue<TestValue> c(1);
    auto start = std::chrono::steady_clock::now();
    c.set({ 2 });
    auto delta = std::chrono::steady_clock::now() - start;
    EXPECT_LT(delta, 1ms);
}

TEST(LiveValue, GetDoesNotBlockWhileSetIsWaiting)
{
    LiveValue<TestValue> c(1);
    std::thread t1([&] {
        auto ref = c.get(125ms);
        ref->x = 2;
        std::this_thread::sleep_for(100ms);
        // Set is waiting, attempt to call get() here. It should not block.
        auto start = std::chrono::steady_clock::now();
        auto second_ref = c.get();
        auto delta = std::chrono::steady_clock::now() - start;
        EXPECT_LT(delta, 1ms);
    });
    std::thread t2([&] {
        std::this_thread::sleep_for(25ms);
        c.set({ 3 });
    });
    t1.join();
    t2.join();
}

TEST(LiveValue, SetBlocksLongEnoughWhenGetIsCalledWhileSetIsBlocking)
{
    LiveValue<TestValue> c(1);
    std::thread t1([&] {
        auto ref = c.get(75ms);
        ref->x = 2;
        std::this_thread::sleep_for(50ms);
        auto second_ref = c.get(175ms);
        std::this_thread::sleep_for(150ms);
    });
    std::thread t2([&] {
        std::this_thread::sleep_for(25ms);
        // set() will wait for the amount of time in max_wait at most
        // at the point in time this method is called, but it should not
        // throw for longer since get() is called again while waiting,
        // which should ultimately increase the timeout for set().
        auto start = std::chrono::steady_clock::now();
        c.set({ 3 });
        auto delta = std::chrono::steady_clock::now() - start;
        EXPECT_GT(delta, 25ms + 150ms - 25ms);
    });
    t1.join();
    t2.join();
}
