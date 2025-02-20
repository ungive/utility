#include <chrono>
#include <thread>
#include <type_traits>
#include <vector>

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
        // Pretend it takes this long to use the value.
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
        std::shared_ptr<const TestValue> copied_ref{ nullptr };
        {
            auto ref = c.get(125ms);
            copied_ref = ref;
        }
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

TEST(LiveValue, DestructingLiveValueBeforeGetReturnValueMaintainsInternalValue)
{
    std::shared_ptr<TestValue> value_ref{ nullptr };
    {
        std::shared_ptr<const TestValue> get_ref{ nullptr };
        {
            LiveValue<TestValue> c(1);
            value_ref = c._value();
            // There are two reference to the value reference:
            // One in c and one in this local variable.
            EXPECT_EQ(2, value_ref.use_count());
            auto ref = c.get();
            // There only exists one reference to values returned by get().
            EXPECT_EQ(1, ref.use_count());
            // The returned reference should contain a copy of the
            // shared pointer in order to keep the pointer alive.
            EXPECT_EQ(3, value_ref.use_count());
            get_ref = ref;
            EXPECT_EQ(2, ref.use_count());
            EXPECT_EQ(2, get_ref.use_count());
            // Incrementing the reference returned by get()
            // should not copy the value reference again.
            EXPECT_EQ(3, value_ref.use_count());
        }
        EXPECT_EQ(1, get_ref.use_count());
        // The LiveValue instance has been destructed,
        // so there's only a reference left in value_ref and get_ref.
        EXPECT_EQ(2, value_ref.use_count());
    }
    EXPECT_EQ(1, value_ref.use_count());
}

TEST(LiveValue, GetReturnsChangedValueAfterUpdatingValueWithSet)
{
    LiveValue<TestValue> c(1);
    c.set({ 2 }); // move
    EXPECT_EQ(2, c.get()->x);
    TestValue other{ 3 };
    c.set(other); // const-reference
    EXPECT_EQ(3, c.get()->x);
    c.set(4); // forward arguments to constructor
    EXPECT_EQ(4, c.get()->x);
}

TEST(LiveValue, GetValueCannotBeModified)
{
    LiveValue<TestValue> c(1);
    auto ref = c.get();
    static_assert(std::is_const_v<decltype(ref)::element_type>);
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
        std::this_thread::sleep_for(50ms);
        auto second_ref = c.get(175ms);
        // Make sure the first reference is not used beyond its lifetime.
        ref = nullptr;
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

template <typename U, typename V>
void log_timestamps(U u, V v)
{
    std::cerr
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count()
        << ": "
        << std::chrono::duration_cast<std::chrono::milliseconds>(u).count()
        << "ms"
        << " // "
        << std::chrono::duration_cast<std::chrono::milliseconds>(v).count()
        << "ms" << std::endl;
}

#ifdef TRACK_LIFETIMES
TEST(LiveValue, LifetimeTrackingCausesDeathWhenGetReturnValueLivesTooLong)
{
    LiveValue<TestValue> c(1);
    auto expected_lifetime = 156ms;
    auto start = std::chrono::steady_clock::now();
    EXPECT_DEATH(
        {
            auto ref = c.get(expected_lifetime);
            std::this_thread::sleep_for(expected_lifetime + 1000ms);
        },
        "");
    auto delta = std::chrono::steady_clock::now() - start;
    log_timestamps(delta, expected_lifetime);
    EXPECT_GT(delta, expected_lifetime);
    EXPECT_LT(delta, expected_lifetime + 50ms);
}

#ifdef LIFETIME_RECORDING
TEST(LiveValue, SetThrowsWhenGetReturnValueLivesBeyondItsLifetime)
{
    LiveValue<TestValue> c(1);
    c._stop_lifetime_tracking();
    auto ref = c.get(100ms);
    auto start = std::chrono::steady_clock::now();
    EXPECT_ANY_THROW(c.set({ 2 }));
    auto delta = std::chrono::steady_clock::now() - start;
    EXPECT_GT(delta, 75ms);
}

TEST(LiveValue, LifetimeTrackingCausesDeathWhenMultipleGetsLiveTooLong)
{
    using namespace std::chrono;

    std::vector<std::thread> threads;
    LiveValue<TestValue> c(1);
    c._record_lifetime_history();

    auto call_get = [&](milliseconds lifetime, bool exceed_lifetime = false) {
        threads.push_back(std::thread([&c, lifetime, exceed_lifetime] {
            auto ref = c.get(lifetime);
            auto sleep_time = lifetime;
            if (exceed_lifetime) {
                sleep_time += 25ms;
            } else {
                sleep_time -= 25ms;
            }
            std::this_thread::sleep_for(sleep_time);
        }));
    };

    auto pop_history = [&] {
        auto entry = c._await_lifetime_history_entry();
        EXPECT_FALSE(entry.second);
    };

    auto expect_history = [&](milliseconds value, bool fail = false) {
        auto entry = c._await_lifetime_history_entry();
        EXPECT_GT(entry.first.count(), (value - 25ms).count());
        EXPECT_LT(entry.first.count(), (value + 25ms).count());
        EXPECT_EQ(entry.second, fail);
    };

    // Singular get() that does not exceed the lifetime.
    call_get(100ms, false);
    pop_history();
    expect_history(100ms);

    // get() that does not exceeded the lifetime with another get()
    // that is called while the first one is still active,
    // but which does exceed the lifetime after the first is destroyed.
    call_get(150ms, false);
    std::this_thread::sleep_for(50ms);
    call_get(150ms, true);
    pop_history();
    expect_history(50ms);
    expect_history(100ms);
    expect_history(50ms, true);

    // get() that does exceed the lifetime, followed by a get() that doesn't,
    // but that is retrieved and destroyed during the lifetime of the first.
    call_get(250ms, true);
    std::this_thread::sleep_for(50ms);
    call_get(75ms, false);
    pop_history();
    expect_history(50ms);
    expect_history(75ms);
    expect_history(125ms, true);

    for (auto& thread : threads) {
        assert(thread.joinable());
        thread.join();
    }
}
#endif // LIFETIME_RECORDING
#endif // TRACK_LIFETIMES
