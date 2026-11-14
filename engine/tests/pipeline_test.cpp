#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>
#include "EnginePipeline.hpp"

using namespace exch;
using Clock = std::chrono::steady_clock;

static void seedUsers(EngineCore& core, unsigned users, Price cash, Qty shares) {
    for (unsigned i = 0; i < users; ++i) {
        const UserId u = core.exchange().accounts().createAccount(cash);
        core.exchange().accounts().grantShares(u, 0, shares, toPaise(500));
    }
}

static Command limitBuy(UserId u, Price px, Qty q, std::uint64_t ref) {
    Command c{};
    c.type      = CommandType::NEW_ORDER;
    c.side      = Side::BUY;
    c.orderType = OrderType::LIMIT;
    c.symbol    = 0;
    c.user      = u;
    c.price     = px;
    c.qty       = q;
    c.clientRef = ref;
    return c;
}

static std::vector<Command> randomCommands(unsigned seed, int n) {
    std::mt19937 rng(seed);
    std::vector<Command> cmds;
    for (int i = 0; i < n; ++i) {
        Command c{};
        c.user      = rng() % 3;
        c.symbol    = 0;
        c.clientRef = static_cast<std::uint64_t>(i);
        const unsigned roll = rng() % 100;
        if (roll < 15) {                                   // cancels, many aimed at ids that don't exist
            c.type    = CommandType::CANCEL_ORDER;
            c.orderId = makeOrderId(0, 1 + rng() % 400);
        } else {
            c.type = CommandType::NEW_ORDER;
            c.side = (rng() & 1) ? Side::BUY : Side::SELL;
            c.qty  = 1 + rng() % 100;
            if (roll < 80) {
                c.orderType = OrderType::LIMIT;
                c.price     = toPaise(490 + rng() % 21);
            } else {
                c.orderType = OrderType::MARKET;
            }
        }
        cmds.push_back(c);
    }
    return cmds;
}

static bool sameEvent(const Event& a, const Event& b) {
    return a.type == b.type && a.reason == b.reason && a.status == b.status &&
           a.side == b.side && a.symbol == b.symbol && a.user == b.user &&
           a.orderId == b.orderId && a.price == b.price && a.qty == b.qty &&
           a.remaining == b.remaining && a.clientRef == b.clientRef &&
           a.trade.tradeId == b.trade.tradeId && a.trade.symbolId == b.trade.symbolId &&
           a.trade.price == b.trade.price && a.trade.qty == b.trade.qty &&
           a.trade.buyOrderId == b.trade.buyOrderId &&
           a.trade.sellOrderId == b.trade.sellOrderId &&
           a.trade.buyerId == b.trade.buyerId && a.trade.sellerId == b.trade.sellerId &&
           a.trade.seq == b.trade.seq;
}

// The threaded engine must behave exactly like calling the core directly.
TEST(Pipeline, MatchesTheDirectCoreEventForEvent) {
    const std::vector<Command> cmds = randomCommands(7, 20000);

    std::vector<Event> expected;
    {
        EngineCore ref(1);
        seedUsers(ref, 3, toPaise(1'000'000), 5000);
        for (const Command& c : cmds)
            ref.process(c, [&](const Event& e) { expected.push_back(e); });
    }
    std::size_t trades = 0;
    for (const Event& e : expected) trades += (e.type == EventType::TRADE);
    ASSERT_GT(trades, 0u);                                 // the workload really trades

    EnginePipeline pipe(1);
    seedUsers(pipe.core(), 3, toPaise(1'000'000), 5000);
    pipe.start();

    std::vector<Event> got;
    got.reserve(expected.size());
    std::size_t sent = 0;
    Event e;
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (got.size() < expected.size() && Clock::now() < deadline) {
        if (sent < cmds.size() && pipe.trySubmit(cmds[sent])) ++sent;
        while (pipe.tryPollEvent(e)) got.push_back(e);
    }
    pipe.stop();

    ASSERT_EQ(sent, cmds.size()) << "timed out while sending";
    ASSERT_EQ(got.size(), expected.size()) << "timed out waiting for events";
    for (std::size_t i = 0; i < got.size(); ++i)
        ASSERT_TRUE(sameEvent(got[i], expected[i])) << "streams diverge at event " << i;

    std::size_t extra = 0;
    while (pipe.tryPollEvent(e)) ++extra;
    EXPECT_EQ(extra, 0u);                                  // no surplus events
    EXPECT_EQ(pipe.commandsProcessed(), cmds.size());
}

TEST(Pipeline, StopProcessesEverythingAlreadySubmitted) {
    EnginePipeline pipe(1);
    seedUsers(pipe.core(), 1, toPaise(1'000'000), 0);
    pipe.start();

    for (std::uint64_t i = 0; i < 1000; ++i)
        ASSERT_TRUE(pipe.trySubmit(limitBuy(0, toPaise(450), 1, i)));
    pipe.stop();                                           // returns only after all 1000 are done

    EXPECT_EQ(pipe.commandsProcessed(), 1000u);
    EXPECT_EQ(pipe.core().exchange().engine().book(0).orderCount(), 1000u);
    std::size_t acks = 0;
    Event e;
    while (pipe.tryPollEvent(e)) acks += (e.type == EventType::ORDER_ACCEPTED);
    EXPECT_EQ(acks, 1000u);
}
// Nobody reads events at first, so the output queue fills and the engine must STALL,
// not drop anything. The producer runs on its own thread and simply waits whenever
// the input queue is full.
TEST(Pipeline, FullOutputQueueStallsTheEngineWithoutLosingEvents) {
    constexpr std::uint64_t kCommands = 150'000;           // one event each: more than the output holds
    static_assert(kCommands > EnginePipeline::kOutCap);

    EnginePipeline pipe(1);
    seedUsers(pipe.core(), 1, toPaise(1'000'000'000), 0);  // Rs 100 crore of cash
    pipe.start();

    std::atomic<bool> giveUp{false};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCommands; ++i) {
            while (!pipe.trySubmit(limitBuy(0, toPaise(450), 1, i))) {
                if (giveUp.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();                 // input queue full: wait
            }
        }
    });

    // 1. With nobody reading events, the engine must stall once the output queue is full.
    const auto stallDeadline = Clock::now() + std::chrono::seconds(20);
    while (pipe.commandsProcessed() < EnginePipeline::kOutCap && Clock::now() < stallDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(pipe.commandsProcessed(), EnginePipeline::kOutCap);

    // 2. Start reading: every event must arrive, in order, with nothing skipped.
    std::uint64_t seen = 0;
    bool inOrder = true;
    Event e;
    const auto drainDeadline = Clock::now() + std::chrono::seconds(20);
    while (seen < kCommands && Clock::now() < drainDeadline) {
        if (!pipe.tryPollEvent(e)) continue;
        if (e.type != EventType::ORDER_ACCEPTED || e.clientRef != seen ||
            localOrderId(e.orderId) != seen + 1) {
            inOrder = false;
            break;
        }
        ++seen;
    }

    giveUp.store(true, std::memory_order_relaxed);         // lets the producer exit if we failed early
    producer.join();                                       // never leave a joinable thread behind
    EXPECT_TRUE(inOrder);
    EXPECT_EQ(seen, kCommands);
    // The pipeline's destructor stops the engine thread (it cannot hang on a full queue).
}