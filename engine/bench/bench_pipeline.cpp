#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>
#include <mutex>
#include "EnginePipeline.hpp"

using namespace exch;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kSymbols = 6;
constexpr unsigned    kUsers   = 1000;
constexpr Price       kMid     = toPaise(500);

struct Totals {
    std::uint64_t events = 0, trades = 0, sharesTraded = 0;
    std::uint64_t orderRejects = 0, cancelsOk = 0, cancelsRejected = 0;
    bool operator==(const Totals& o) const {
        return events == o.events && trades == o.trades && sharesTraded == o.sharesTraded &&
               orderRejects == o.orderRejects && cancelsOk == o.cancelsOk &&
               cancelsRejected == o.cancelsRejected;
    }
};

inline void tally(const Event& e, Totals& t) {
    ++t.events;
    switch (e.type) {
        case EventType::TRADE:
            ++t.trades;
            t.sharesTraded += static_cast<std::uint64_t>(e.trade.qty);
            break;
        case EventType::ORDER_REJECTED:  ++t.orderRejects;    break;
        case EventType::ORDER_CANCELLED: ++t.cancelsOk;       break;
        case EventType::CANCEL_REJECTED: ++t.cancelsRejected; break;
        case EventType::ORDER_ACCEPTED:  break;
    }
}

// Same workload as the default one in bench_engine.cpp (same seed, same random calls).
std::vector<Command> generateCommands(std::size_t n, unsigned seed) {
    struct Placed { UserId user; SymbolId symbol; OrderId id; };
    std::mt19937 rng(seed);
    std::vector<Command> cmds;
    std::vector<Placed> placed;
    std::vector<std::uint64_t> seq(kSymbols, 0);
    cmds.reserve(n);
    placed.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        const unsigned roll = rng() % 100;
        Command c{};
        c.clientRef = i;

        if (roll < 10 && !placed.empty()) {                       // 10% cancel
            const Placed& p = placed[rng() % placed.size()];
            c.type    = CommandType::CANCEL_ORDER;
            c.user    = p.user;
            c.symbol  = p.symbol;
            c.orderId = p.id;
            cmds.push_back(c);
            continue;
        }

        const UserId   user   = rng() % kUsers;
        const SymbolId symbol = static_cast<SymbolId>(rng() % kSymbols);
        const Side     side   = (rng() & 1) ? Side::BUY : Side::SELL;
        const Qty      qty    = 1 + rng() % 200;
        const Price    px     = kMid + (static_cast<Price>(rng() % 41) - 20) * 5;
        const bool     isLimit = roll < 80;                       // 70% limit, 20% market

        c.type      = CommandType::NEW_ORDER;
        c.side      = side;
        c.orderType = isLimit ? OrderType::LIMIT : OrderType::MARKET;
        c.symbol    = symbol;
        c.user      = user;
        c.price     = isLimit ? px : 0;
        c.qty       = qty;
        cmds.push_back(c);
        placed.push_back({user, symbol, makeOrderId(symbol, ++seq[symbol])});
    }
    return cmds;
}

void seed(EngineCore& core) {
    for (unsigned u = 0; u < kUsers; ++u) {
        const UserId id = core.exchange().accounts().createAccount(toPaise(100'000'000));
        for (SymbolId s = 0; s < kSymbols; ++s)
            core.exchange().accounts().grantShares(id, s, 1'000'000, toPaise(500));
    }
}

double runDirect(const std::vector<Command>& cmds, Totals& t) {
    EngineCore core(kSymbols);
    seed(core);
    const auto start = Clock::now();
    for (const Command& c : cmds)
        core.process(c, [&t](const Event& e) { tally(e, t); });
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Producer thread -> in queue -> engine thread -> out queue -> this thread.
// Returns seconds, or a negative number if it timed out.
double runPipeline(const std::vector<Command>& cmds, std::uint64_t expectedEvents, Totals& t) {
    EnginePipeline pipe(kSymbols);
    seed(pipe.core());
    pipe.start();

    std::atomic<bool> giveUp{false};
    const auto start    = Clock::now();
    const auto deadline = start + std::chrono::seconds(60);

    std::thread producer([&] {
        for (const Command& c : cmds)
            while (!pipe.trySubmit(c)) {
                if (giveUp.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();                        // input queue full: wait
            }
    });

    Event e;
    bool timedOut = false;
    while (t.events < expectedEvents) {
        if (pipe.tryPollEvent(e)) { tally(e, t); continue; }
        if (Clock::now() > deadline) { timedOut = true; break; }
        std::this_thread::yield();                                // output empty: wait
    }
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();

    giveUp.store(true, std::memory_order_relaxed);
    producer.join();
    return timedOut ? -1.0 : secs;                // the destructor stops the engine thread
}

// Submit one command, wait for its event, repeat: the cost of one hand-off when idle.
std::vector<std::uint32_t> runRoundTrips(std::size_t n) {
    EnginePipeline pipe(kSymbols);
    seed(pipe.core());
    pipe.start();

    std::vector<std::uint32_t> lat;
    lat.reserve(n);
    Event e;
    for (std::size_t i = 0; i < n; ++i) {
        Command c{};
        c.type      = CommandType::NEW_ORDER;
        c.side      = Side::BUY;
        c.orderType = OrderType::LIMIT;
        c.symbol    = 0;
        c.user      = 0;
        c.price     = kMid - static_cast<Price>(1 + i % 1000) * 5;   // far from the spread: no trade
        c.qty       = 1;
        c.clientRef = i;

        const auto a = Clock::now();
        while (!pipe.trySubmit(c)) {}
        while (!pipe.tryPollEvent(e)) {}
        const auto b = Clock::now();
        lat.push_back(static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    }
    return lat;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::uint32_t percentile(const std::vector<std::uint32_t>& sorted, double p) {
    return sorted[static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1))];
}
// A simple test-and-test-and-set spinlock, to compare with std::mutex.
class SpinLock {
public:
    void lock() {
        for (;;) {
            if (!flag_.exchange(true, std::memory_order_acquire)) return;
            while (flag_.load(std::memory_order_relaxed)) { /* spin until it looks free */ }
        }
    }
    void unlock() { flag_.store(false, std::memory_order_release); }
private:
    std::atomic<bool> flag_{false};
};

// Each thread's counters get their own cache line, so threads don't slow each other
// down just by counting.
struct alignas(64) PaddedTotals { Totals t; };

// `threads` threads share ONE EngineCore behind a lock. Thread k handles commands
// k, k+threads, k+2*threads, ... so the total work is the same for every thread count.
template <typename Lock>
double runLocked(const std::vector<Command>& cmds, unsigned threads,
                 Totals& total, bool& invariantsOk) {
    EngineCore core(kSymbols);
    seed(core);
    Lock lock;
    std::atomic<bool> go{false};
    std::vector<PaddedTotals> mine(threads);
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < threads; ++k) {
        pool.emplace_back([&, k] {
            while (!go.load(std::memory_order_acquire)) { /* wait for the start signal */ }
            Totals& t = mine[k].t;
            for (std::size_t i = k; i < cmds.size(); i += threads) {
                std::lock_guard<Lock> guard(lock);
                core.process(cmds[i], [&t](const Event& e) { tally(e, t); });
            }
        });
    }
    const auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (std::thread& th : pool) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();

    for (const PaddedTotals& m : mine) {
        total.events          += m.t.events;
        total.trades          += m.t.trades;
        total.sharesTraded    += m.t.sharesTraded;
        total.orderRejects    += m.t.orderRejects;
        total.cancelsOk       += m.t.cancelsOk;
        total.cancelsRejected += m.t.cancelsRejected;
    }

    // Nothing may be created, destroyed or driven negative, whatever the interleaving.
    const AccountManager& acc = core.exchange().accounts();
    invariantsOk = (acc.totalCash() == static_cast<Price>(kUsers) * toPaise(100'000'000));
    for (SymbolId s = 0; s < kSymbols; ++s)
        invariantsOk = invariantsOk && (acc.totalShares(s) == static_cast<Qty>(kUsers) * 1'000'000);
    for (UserId u = 0; u < kUsers; ++u) {
        const Account& a = acc.account(u);
        invariantsOk = invariantsOk && a.cash >= 0 && a.reservedCash >= 0 && a.reservedCash <= a.cash;
        for (const Position& p : a.positions)
            invariantsOk = invariantsOk && p.qty >= 0 && p.reservedQty >= 0 && p.reservedQty <= p.qty;
    }
    return secs;
}

template <typename Lock>
void benchLocked(const char* name, const std::vector<Command>& cmds,
                 const Totals& ref, double directOps) {
    for (unsigned threads : {1u, 2u, 4u, 8u}) {
        std::vector<double> secs;
        bool ok = true;
        for (int round = 0; round < 3; ++round) {
            Totals t;
            bool inv = false;
            secs.push_back(runLocked<Lock>(cmds, threads, t, inv));
            ok = ok && inv;
            if (threads == 1 && !(t == ref)) ok = false;      // one thread must equal the direct run
        }
        const double ops = static_cast<double>(cmds.size()) / median(secs);
        std::printf("  %-10s %u thread%s : %10.0f commands/sec  (%3.0f%% of direct)  checks %s\n",
                    name, threads, threads == 1 ? " " : "s", ops,
                    100.0 * ops / directOps, ok ? "ok" : "FAILED");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 1'000'000;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);

#ifndef NDEBUG
    std::puts("WARNING: asserts are ON (not a Release build). These numbers are not valid.");
#endif
    std::printf("Pipeline benchmark: %zu commands (70%% limit, 20%% market, 10%% cancel), "
                "%zu symbols, %u users, seed 42\n", n, kSymbols, kUsers);
    std::printf("Hardware threads: %u (the pipeline runs 3 busy threads: producer, engine, consumer)\n\n",
                std::thread::hardware_concurrency());

    const std::vector<Command> cmds = generateCommands(n, 42);

    Totals warmDirect, warmPipe;                        // warm-up, results discarded
    runDirect(cmds, warmDirect);
    runPipeline(cmds, warmDirect.events, warmPipe);

    std::vector<double> direct, piped;
    Totals reference;
    bool ok = true;
    for (int round = 0; round < 3; ++round) {
        Totals td, tp;
        direct.push_back(runDirect(cmds, td));
        const double s = runPipeline(cmds, td.events, tp);
        piped.push_back(s);
        if (s < 0 || !(td == tp)) ok = false;
        reference = td;
    }

    const double dOps = static_cast<double>(n) / median(direct);
    const double pOps = static_cast<double>(n) / median(piped);
    std::printf("Direct call (one thread)         : %.0f commands/sec  (median of 3 runs)\n", dOps);
    std::printf("Pipeline (3 threads, 2 queues)   : %.0f commands/sec  (median of 3 runs)\n", pOps);
    std::printf("Pipeline as %% of direct          : %.0f%%\n", 100.0 * pOps / dOps);
    std::printf("Pipeline events == direct events : %s\n\n", ok ? "yes, identical totals every round" : "NO - FAILED");

    std::vector<std::uint32_t> lat = runRoundTrips(100'000);
    std::sort(lat.begin(), lat.end());
    std::printf("Round trip, idle pipeline (submit one command, wait for its event):\n");
    std::printf("  ns: p50 %u | p90 %u | p99 %u | p99.9 %u | max %u\n\n",
                percentile(lat, 50), percentile(lat, 90), percentile(lat, 99),
                percentile(lat, 99.9), lat.back());

    std::printf("Fingerprint:\n");
    std::printf("  events          : %llu\n", static_cast<unsigned long long>(reference.events));
    std::printf("  trades          : %llu\n", static_cast<unsigned long long>(reference.trades));
    std::printf("  shares traded   : %llu\n", static_cast<unsigned long long>(reference.sharesTraded));
    std::printf("  order rejects   : %llu\n", static_cast<unsigned long long>(reference.orderRejects));
    std::printf("  cancels ok      : %llu\n", static_cast<unsigned long long>(reference.cancelsOk));
    std::printf("  cancels refused : %llu\n", static_cast<unsigned long long>(reference.cancelsRejected));
    if (n == 1'000'000)
        std::printf("  matches the earlier engine benchmark (862414 / 43550973 / 3362): %s\n",
                    (reference.trades == 862414 && reference.sharesTraded == 43550973 &&
                     reference.cancelsOk == 3362) ? "yes" : "NO");
        // ---- single writer vs locks: same commands, cancels removed ----
    std::vector<Command> noCancels;
    noCancels.reserve(cmds.size());
    for (const Command& c : cmds)
        if (c.type == CommandType::NEW_ORDER) noCancels.push_back(c);

    std::vector<double> directSecs, pipeSecs;
    Totals ref2;
    bool ok2 = true;
    for (int round = 0; round < 3; ++round) {
        Totals td, tp;
        directSecs.push_back(runDirect(noCancels, td));
        const double s = runPipeline(noCancels, td.events, tp);
        pipeSecs.push_back(s);
        if (s < 0 || !(td == tp)) ok2 = false;
        ref2 = td;
    }
    const double dOps2 = static_cast<double>(noCancels.size()) / median(directSecs);
    const double pOps2 = static_cast<double>(noCancels.size()) / median(pipeSecs);

    std::printf("\nSingle writer vs locks: %zu commands (cancels removed), 3 runs each, median\n",
                noCancels.size());
    std::printf("  direct, 1 thread, no lock    : %10.0f commands/sec\n", dOps2);
    std::printf("  pipeline, 1 producer         : %10.0f commands/sec  (%3.0f%% of direct)  checks %s\n",
                pOps2, 100.0 * pOps2 / dOps2, ok2 ? "ok" : "FAILED");
    benchLocked<std::mutex>("std::mutex", noCancels, ref2, dOps2);
    benchLocked<SpinLock>("spinlock", noCancels, ref2, dOps2);
    std::printf("  With 2+ locked threads the totals differ run to run (random arrival order), so\n");
    std::printf("  \"checks\" means: cash and shares conserved, no negative balance or reservation.\n");
    return ok && ok2 ? 0 : 1;
}