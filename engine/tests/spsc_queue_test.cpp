#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <thread>
#include "SpscQueue.hpp"

using namespace exch;

TEST(SpscQueue, StartsEmpty) {
    SpscQueue<int, 4> q;
    int out = -1;
    EXPECT_FALSE(q.tryPop(out));
    EXPECT_EQ(out, -1);                       // left untouched
}

TEST(SpscQueue, PreservesFifoOrder) {
    SpscQueue<int, 8> q;
    for (int i = 1; i <= 3; ++i) EXPECT_TRUE(q.tryPush(i));
    int out = 0;
    for (int i = 1; i <= 3; ++i) {
        ASSERT_TRUE(q.tryPop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(q.tryPop(out));
}

TEST(SpscQueue, RejectsPushWhenFull) {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.tryPush(i));   // all 4 slots are usable
    EXPECT_FALSE(q.tryPush(99));                             // full: must not overwrite
    int out = 0;
    ASSERT_TRUE(q.tryPop(out));
    EXPECT_EQ(out, 0);                                       // oldest item is intact
    EXPECT_TRUE(q.tryPush(99));                              // space again
}

TEST(SpscQueue, WrapsAroundManyTimes) {
    SpscQueue<std::uint64_t, 4> q;
    std::uint64_t next = 0, expect = 0;
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.tryPush(next++));
        std::uint64_t out = 0;
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(q.tryPop(out));
            ASSERT_EQ(out, expect++);
        }
    }
}

TEST(SpscQueue, SizeTracksContents) {
    SpscQueue<int, 8> q;
    EXPECT_EQ(q.sizeApprox(), 0u);
    q.tryPush(1);
    q.tryPush(2);
    EXPECT_EQ(q.sizeApprox(), 2u);
    int out = 0;
    q.tryPop(out);
    EXPECT_EQ(q.sizeApprox(), 1u);
}

// ---- two real threads ----

// `check` is the bitwise inverse of `seq`, so a half-written message is detectable.
struct Msg { std::uint64_t seq; std::uint64_t check; };

// Producer sends 0..count-1; consumer verifies order and integrity. Returns #bad messages.
template <std::size_t N>
static std::uint64_t transfer(std::uint64_t count) {
    auto q = std::make_unique<SpscQueue<Msg, N>>();

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            const Msg m{i, ~i};
            while (!q->tryPush(m)) { /* full: spin until the consumer catches up */ }
        }
    });

    std::uint64_t expected = 0, bad = 0;
    while (expected < count) {
        Msg m{};
        if (!q->tryPop(m)) continue;                          // empty: spin
        if (m.seq != expected || m.check != ~m.seq) ++bad;
        ++expected;
    }
    producer.join();
    return bad;
}

TEST(SpscQueue, TwoThreadsKeepEveryMessageInOrder) {
    EXPECT_EQ(transfer<1024>(1'000'000), 0u);
}

TEST(SpscQueue, TwoThreadsOnATinyQueueForceConstantFullAndEmpty) {
    EXPECT_EQ(transfer<2>(200'000), 0u);
}