#include <chrono>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// #define NO_TRACK_LIFETIMES
#include "ungive/utility/atomic.h"

using namespace testing;
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

using WaitCodepath = Atomic<TestValue>::WaitCodepath;

inline void expect_wait_codepath(Atomic<TestValue>& c, WaitCodepath path)
{
    static_assert(
        std::is_same_v<std::remove_reference_t<decltype(c)>::WaitCodepath,
            decltype(path)>);
#ifdef WAIT_CODEPATHS
    EXPECT_THAT(c._wait_codepaths(), Contains(path));
#endif
}

TEST(Atomic, Example)
{
    struct Data
    {
        int x = 1;
    };

    Atomic<Data> value;
    bool watch_called{ false };

    // all of the following statements are thread-safe:
    value.watch([&](Data const& data) {
        // called during each set() call
        EXPECT_EQ(2, data.x); // returns 2
        watch_called = true;
    });
    EXPECT_EQ(1, value.get()->x); // returns 1 (blocks any set calls)
    value.set({ 2 }); // sets x to 2 (blocking)
    EXPECT_EQ(2, value.get()->x); // returns 2
    // auto ref = value.get(); // do not do this
    EXPECT_TRUE(watch_called);
}

TEST(Atomic, ConstructWithDefaultConstructor)
{
    Atomic<TestValue> c;
    EXPECT_EQ(TEST_VALUE_DEFAULT_X, c.get()->x);
}

TEST(Atomic, ConstructWithMoveConstructor)
{
    int expected = 102;
    TestValue value{ expected };
    Atomic<TestValue> c(std::move(value));
    EXPECT_EQ(expected, c.get()->x);
}

TEST(Atomic, ConstructWithCopyConstructor)
{
    TestValue value{ 103 };
    Atomic<TestValue> c(value);
    EXPECT_EQ(value.x, c.get()->x);
}

TEST(Atomic, ConstructWithInitConstructor)
{
    int expected = 104;
    Atomic<TestValue> c(104);
    EXPECT_EQ(expected, c.get()->x);
}

TEST(Atomic, SetBlocksUntilGetReturnValueIsDestructed)
{
    Atomic<TestValue> c(1);
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

TEST(Atomic, CopyOfGetReturnValueMakesSetBlockAsWell)
{
    Atomic<TestValue> c(1);
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

TEST(Atomic, DestructingLiveValueBeforeGetReturnValueMaintainsInternalValue)
{
    std::shared_ptr<const TestValue> value_ref{ nullptr };
    {
        std::shared_ptr<const TestValue> get_ref{ nullptr };
        {
            Atomic<TestValue> c(1);
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
        // The Atomic instance has been destructed,
        // so there's only a reference left in value_ref and get_ref.
        EXPECT_EQ(2, value_ref.use_count());
    }
    EXPECT_EQ(1, value_ref.use_count());
}

TEST(Atomic, GetReturnsChangedValueAfterUpdatingValueWithSet)
{
    Atomic<TestValue> c(1);
    c.set({ 2 }); // move
    EXPECT_EQ(2, c.get()->x);
    TestValue other{ 3 };
    c.set(other); // const-reference
    EXPECT_EQ(3, c.get()->x);
    c.set(4); // forward arguments to constructor
    EXPECT_EQ(4, c.get()->x);
}

TEST(Atomic, GetValueCannotBeModified)
{
    Atomic<TestValue> c(1);
    auto ref = c.get();
    static_assert(std::is_const_v<decltype(ref)::element_type>);
}

TEST(Atomic, SetDoesNotBlockWhenGetWasNeverCalled)
{
    Atomic<TestValue> c(1);
    auto start = std::chrono::steady_clock::now();
    c.set({ 2 });
    auto delta = std::chrono::steady_clock::now() - start;
    EXPECT_LT(delta, 1ms);
}

TEST(Atomic, GetDoesNotBlockWhileSetIsWaiting)
{
    Atomic<TestValue> c(1);
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

TEST(Atomic, SetBlocksLongEnoughWhenGetIsCalledWhileSetIsBlocking)
{
    Atomic<TestValue> c(1);
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
    expect_wait_codepath(c, WaitCodepath::UpdatedDeadline);
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

TEST(Atomic, SetThrowsWhenGetReturnValueLivesBeyondItsLifetime)
{
    // FIXME this test should run even without lifetime tracking/recording!
    Atomic<TestValue> c(1);
#ifdef TRACK_LIFETIMES
    c._stop_lifetime_tracking();
#endif
    auto ref = c.get(100ms);
    auto start = std::chrono::steady_clock::now();
    EXPECT_ANY_THROW(c.set({ 2 }));
    auto delta = std::chrono::steady_clock::now() - start;
    EXPECT_GT(delta, 75ms);
}

#ifdef TRACK_LIFETIMES
TEST(Atomic, LifetimeTrackingCausesDeathWhenGetReturnValueLivesTooLong)
{
    Atomic<TestValue> c(1);
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
TEST(Atomic, LifetimeTrackingCausesDeathWhenMultipleGetsLiveTooLong)
{
    using namespace std::chrono;

    std::vector<std::thread> threads;
    Atomic<TestValue> c(1);
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
        EXPECT_GE(entry.first.count(), (value - 25ms).count());
        EXPECT_LE(entry.first.count(), (value + 25ms).count());
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

TEST(Atomic, SetCallsWatchCallback)
{
    Atomic<TestValue> c(1);
    TestValue watch_result{};
    c.watch([&](TestValue const& value) {
        watch_result = value;
    });
    c.set(2);
    EXPECT_EQ(2, watch_result.x);
}

TEST(Atomic, WatchCallbackCannotCallInstanceMethods)
{
    Atomic<TestValue> c(1);
    c.watch([&](TestValue const& value) {
        EXPECT_ANY_THROW(c.get());
        EXPECT_ANY_THROW(c.set({ 3 }));
    });
    c.set(2);
}

TEST(Atomic, ValueTypeIsTheTypeOfTheValue)
{
    Atomic<TestValue> c(1);
    static_assert(std::is_same_v<TestValue, decltype(c)::value_type>);
}

TEST(Atomic, DefaultGetLifetimeIsValuePassedAsTemplateArgument)
{
    constexpr size_t value = 4516;
    Atomic<TestValue, value> c(1);
    EXPECT_EQ(std::chrono::milliseconds{ value }, c.default_get_lifetime);
}

TEST(Atomic, PassedDefaultGetLifeTimeIsUsedAsLifetimeForGetWithoutArguments)
{
    Atomic<TestValue, 163> c(1);
#ifdef TRACK_LIFETIMES
    c._stop_lifetime_tracking();
#endif
    auto ref = c.get();
    auto start = std::chrono::steady_clock::now();
    EXPECT_ANY_THROW(c.set({ 2 }));
    auto delta = std::chrono::steady_clock::now() - start;
    EXPECT_GT(delta, c.default_get_lifetime);
    EXPECT_LT(delta, c.default_get_lifetime + 25ms);
}

TEST(Atomic, SetPrioritizesDataFromTheLatestCall)
{
    constexpr auto expected = 100;
    Atomic<TestValue> c(1);
    std::thread t1([&] {
        auto ref = c.get(125ms);
        std::this_thread::sleep_for(100ms);
    });
    std::thread t2([&] {
        std::this_thread::sleep_for(25ms);
        EXPECT_FALSE(c.set({ 2 }));
    });
    std::thread t3([&] {
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(c.set({ 3 }));
    });
    std::thread t4([&] {
        std::this_thread::sleep_for(75ms);
        EXPECT_TRUE(c.set({ expected }));
    });
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    EXPECT_EQ(expected, c.get()->x);
    expect_wait_codepath(c, WaitCodepath::SetWithLatestData);
    expect_wait_codepath(c, WaitCodepath::NoSetWithOutdatedData);
}

TEST(Atomic, ContainsDataFromLatestSetCallAfterEachOtherBlockingSetReturned)
{
    constexpr auto expected = 100;
    Atomic<TestValue> c(1);
    std::thread t1([&] {
        auto ref = c.get(125ms);
        std::this_thread::sleep_for(100ms);
    });
    std::thread t2([&] {
        std::this_thread::sleep_for(25ms);
        EXPECT_FALSE(c.set({ 2 }));
        EXPECT_EQ(expected, c.get()->x);
    });
    std::thread t3([&] {
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(c.set({ 3 }));
        EXPECT_EQ(expected, c.get()->x);
    });
    std::thread t4([&] {
        std::this_thread::sleep_for(75ms);
        EXPECT_TRUE(c.set({ expected }));
        EXPECT_EQ(expected, c.get()->x);
    });
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    expect_wait_codepath(c, WaitCodepath::SetWithLatestData);
    expect_wait_codepath(c, WaitCodepath::NoSetWithOutdatedData);
}

TEST(Atomic, SetPrioritizesDataFromTheLatestCallWithManyThreads)
{
    constexpr auto expected = 100;
    constexpr auto n_threads = 32;
    // The threads shouldn't set the value to the expected one.
    assert(n_threads < expected / 2);
    std::random_device dev;
    std::mt19937 rng(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist(1, 50);
    Atomic<TestValue> c(1);
    std::vector<std::thread> threads;
    threads.push_back(std::thread([&] {
        auto ref = c.get(125ms);
        std::this_thread::sleep_for(100ms);
    }));
    std::mutex mutex;
    std::condition_variable cv;
    size_t n_ready{ 0 };
    for (int i = 0; i < n_threads; i++) {
        threads.push_back(std::thread([&] {
            auto n = dist(rng);
            std::this_thread::sleep_for(std::chrono::milliseconds{ n });
            {
                std::lock_guard lock(mutex);
                n_ready += 1;
                cv.notify_one();
            }
            EXPECT_FALSE(c.set({ int(2) + i })); // start with 2
            EXPECT_EQ(expected, c.get()->x);
        }));
    }
    {
        std::unique_lock lock(mutex);
        auto ok = cv.wait_for(lock, 1s, [&] {
            return n_ready == n_threads;
        });
        ASSERT_TRUE(ok);
    }
    std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(c.set({ expected }));
    EXPECT_EQ(expected, c.get()->x);
    for (auto& thread : threads) {
        assert(thread.joinable());
        thread.join();
    }
    expect_wait_codepath(c, WaitCodepath::SetWithLatestData);
    expect_wait_codepath(c, WaitCodepath::NoSetWithOutdatedData);
}

TEST(Atomic, OutdatedAndLatestSetCallsTimeOutAndThrowWhenGetLivesTooLong)
{
    Atomic<TestValue> c(1);
#ifdef TRACK_LIFETIMES
    c._stop_lifetime_tracking();
#endif
    std::thread t1([&] {
        auto ref = c.get(125ms);
        std::this_thread::sleep_for(150ms);
    });
    std::thread t2([&] {
        std::this_thread::sleep_for(25ms);
        EXPECT_ANY_THROW(c.set({ 2 }));
    });
    std::thread t3([&] {
        std::this_thread::sleep_for(75ms);
        EXPECT_ANY_THROW(c.set({ 3 }));
    });
    t1.join();
    t2.join();
    t3.join();
    EXPECT_EQ(1, c.get()->x);
    expect_wait_codepath(c, WaitCodepath::NoSetTimeoutLatest);
    expect_wait_codepath(c, WaitCodepath::NoSetTimeoutOtherLatest);
}

// TODO write a test for difficult to trigger codepath: NoSetDelayOtherLatest
// TODO make _stop_lifetime_tracking a noop when TRACK_LIFETIMES is not defined
