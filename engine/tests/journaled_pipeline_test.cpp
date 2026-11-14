#include <gtest/gtest.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "JournaledPipeline.hpp"

using namespace exch;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

class TempFile {
public:
    explicit TempFile(const std::string& tag)
        : path_((fs::temp_directory_path() /
                 ("exch_" + tag + "_" + std::to_string(::getpid()) + ".journal")).string()) {
        fs::remove(path_);
    }
    ~TempFile() { std::error_code ec; fs::remove(path_, ec); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

std::vector<Command> randomCommands(unsigned seed, int n, unsigned users) {
    std::mt19937 rng(seed);
    std::vector<Command> cmds;
    for (int i = 0; i < n; ++i) {
        Command c{};
        c.user      = rng() % users;
        c.symbol    = 0;
        c.clientRef = static_cast<std::uint64_t>(i);
        const unsigned roll = rng() % 100;
        if (roll < 15) {
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

void seedPlain(EngineCore& core) {
    for (int i = 0; i < 3; ++i) {
        const UserId u = core.exchange().accounts().createAccount(toPaise(1'000'000));
        core.exchange().accounts().grantShares(u, 0, 5000, toPaise(500));
    }
}

void seedJournaled(JournaledCore& j) {
    for (int i = 0; i < 3; ++i) {
        const UserId u = j.createAccount(toPaise(1'000'000));
        j.grantShares(u, 0, 5000, toPaise(500));
    }
}

bool sameEvent(const Event& a, const Event& b) {
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

void expectSameState(const EngineCore& a, const EngineCore& b, unsigned users) {
    for (UserId u = 0; u < users; ++u) {
        const Account& x = a.exchange().accounts().account(u);
        const Account& y = b.exchange().accounts().account(u);
        EXPECT_EQ(x.cash, y.cash) << "user " << u;
        EXPECT_EQ(x.reservedCash, y.reservedCash) << "user " << u;
        ASSERT_EQ(x.positions.size(), y.positions.size());
        for (std::size_t s = 0; s < x.positions.size(); ++s) {
            EXPECT_EQ(x.positions[s].qty, y.positions[s].qty) << "user " << u;
            EXPECT_EQ(x.positions[s].reservedQty, y.positions[s].reservedQty) << "user " << u;
            EXPECT_EQ(x.positions[s].costBasis, y.positions[s].costBasis) << "user " << u;
        }
    }
    const OrderBook& ba = a.exchange().engine().book(0);
    const OrderBook& bb = b.exchange().engine().book(0);
    EXPECT_EQ(ba.orderCount(), bb.orderCount());
    EXPECT_TRUE(ba.depth(Side::BUY, 10000) == bb.depth(Side::BUY, 10000));
    EXPECT_TRUE(ba.depth(Side::SELL, 10000) == bb.depth(Side::SELL, 10000));
}

std::uint64_t countCommandRecords(const std::string& path) {
    std::uint64_t n = 0;
    readJournal(path, [&](const JournalRecord& r) { if (r.kind == RecordKind::COMMAND) ++n; });
    return n;
}

// Submit every command and collect `wanted` events. Returns false on timeout.
bool pump(JournaledPipeline& pipe, const std::vector<Command>& cmds,
          std::size_t wanted, std::vector<Event>& got) {
    std::size_t sent = 0;
    Event e;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (got.size() < wanted && Clock::now() < deadline) {
        if (sent < cmds.size() && pipe.trySubmit(cmds[sent])) ++sent;
        while (pipe.tryPollEvent(e)) got.push_back(e);
    }
    return sent == cmds.size() && got.size() == wanted;
}

}  // namespace

TEST(JournaledPipeline, MatchesTheDirectCoreEventForEvent) {
    TempFile file("jp_match");
    const std::vector<Command> cmds = randomCommands(7, 20000, 3);

    std::vector<Event> expected;
    {
        EngineCore ref(1);
        seedPlain(ref);
        for (const Command& c : cmds)
            ref.process(c, [&](const Event& e) { expected.push_back(e); });
    }
    std::size_t trades = 0;
    for (const Event& e : expected) trades += (e.type == EventType::TRADE);
    ASSERT_GT(trades, 0u);

    JournaledPipeline pipe(1, file.path());
    seedJournaled(pipe.core());
    pipe.start();
    std::vector<Event> got;
    got.reserve(expected.size());
    const bool finished = pump(pipe, cmds, expected.size(), got);
    pipe.stop();

    ASSERT_TRUE(finished) << "timed out";
    for (std::size_t i = 0; i < got.size(); ++i)
        ASSERT_TRUE(sameEvent(got[i], expected[i])) << "streams diverge at event " << i;
    EXPECT_FALSE(pipe.failed());
    EXPECT_EQ(pipe.commandsProcessed(), cmds.size());
    EXPECT_EQ(countCommandRecords(file.path()), cmds.size());     // every command is in the journal
}

// The write-ahead rule: when the consumer sees the first event of command k,
// the journal file must ALREADY contain command k.
TEST(JournaledPipeline, NoEventIsVisibleBeforeItsCommandIsInTheJournal) {
    TempFile file("jp_wal");
    const std::vector<Command> cmds = randomCommands(31, 400, 3);

    JournaledPipeline pipe(1, file.path());
    seedJournaled(pipe.core());
    pipe.start();

    std::size_t sent = 0, violations = 0, checked = 0;
    std::uint64_t acked = 0;
    Event e;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (acked < cmds.size() && Clock::now() < deadline) {
        if (sent < cmds.size() && pipe.trySubmit(cmds[sent])) ++sent;
        while (pipe.tryPollEvent(e)) {
            if (e.type == EventType::TRADE) continue;             // trades follow their command's first event
            if (countCommandRecords(file.path()) < e.clientRef + 1) ++violations;
            ++checked;
            acked = e.clientRef + 1;
        }
    }
    pipe.stop();

    EXPECT_EQ(acked, cmds.size());
    EXPECT_EQ(checked, cmds.size());                              // one first-event per command
    EXPECT_EQ(violations, 0u);
}

TEST(JournaledPipeline, RestartRecoversAndTheEnginesKeepBehavingIdentically) {
    TempFile file("jp_restart");
    const std::vector<Command> first = randomCommands(41, 4000, 3);
    const std::vector<Command> more  = randomCommands(42, 2000, 3);

    EngineCore ref(1);
    seedPlain(ref);
    for (const Command& c : first) ref.process(c, [](const Event&) {});

    {
        JournaledPipeline pipe(1, file.path());
        seedJournaled(pipe.core());
        pipe.start();
        for (const Command& c : first)
            while (!pipe.trySubmit(c)) std::this_thread::yield();
        pipe.stop();
    }                                                  // the pipeline is gone; only the journal file remains

    JournaledPipeline pipe(1, file.path());
    EXPECT_EQ(pipe.recovery().read.status, JournalStatus::CLEAN);
    EXPECT_EQ(pipe.recovery().read.records, 7u + first.size());   // 1 price + 3 accounts + 3 grants + commands
    expectSameState(ref, pipe.core().core(), 3);

    std::vector<Event> expected;
    for (const Command& c : more)
        ref.process(c, [&](const Event& e) { expected.push_back(e); });

    pipe.start();
    std::vector<Event> got;
    got.reserve(expected.size());
    const bool finished = pump(pipe, more, expected.size(), got);
    pipe.stop();

    ASSERT_TRUE(finished) << "timed out";
    std::size_t trades = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        ASSERT_TRUE(sameEvent(got[i], expected[i])) << "streams diverge at event " << i;
        trades += (got[i].type == EventType::TRADE);
    }
    EXPECT_GT(trades, 0u);                             // the comparison is not vacuous
}

TEST(JournaledPipeline, DiskSyncModeJournalsEverything) {
    TempFile file("jp_sync");
    const std::vector<Command> cmds = randomCommands(51, 300, 3);
    {
        JournaledPipeline pipe(1, file.path(), /*syncToDisk=*/true);
        seedJournaled(pipe.core());
        pipe.start();
        for (const Command& c : cmds)
            while (!pipe.trySubmit(c)) std::this_thread::yield();
        pipe.stop();
        EXPECT_EQ(pipe.commandsProcessed(), cmds.size());
        EXPECT_FALSE(pipe.failed());
    }
    EXPECT_EQ(countCommandRecords(file.path()), cmds.size());
}