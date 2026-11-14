#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "Exchange.hpp"
#include <string>

using namespace exch;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kSymbols = 6;
constexpr unsigned    kUsers   = 1000;
constexpr Price       kMid     = toPaise(500);

struct Op {
    enum class Kind : std::uint8_t { LIMIT, MARKET, CANCEL };
    Kind     kind;
    Side     side;
    UserId   user;
    SymbolId symbol;
    Price    price;
    Qty      qty;
    OrderId  cancelId;
};

struct Totals {
    std::uint64_t trades       = 0;
    std::uint64_t sharesTraded = 0;
    std::uint64_t rejects      = 0;
    std::uint64_t cancelsOk    = 0;
    bool operator==(const Totals& o) const {
        return trades == o.trades && sharesTraded == o.sharesTraded &&
               rejects == o.rejects && cancelsOk == o.cancelsOk;
    }
};

// Build the whole workload up front so RNG cost is never measured.
std::vector<Op> generateOps(std::size_t n, unsigned seed) {
    struct Placed { UserId user; SymbolId symbol; OrderId id; };
    std::mt19937 rng(seed);
    std::vector<Op> ops;
    std::vector<Placed> placed;
    std::vector<std::uint64_t> seq(kSymbols, 0);          // index k  <->  order id k + 1
    ops.reserve(n);
    placed.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        const unsigned roll = rng() % 100;

        if (roll < 10 && !placed.empty()) {                       // 10% cancel
            const std::size_t k = rng() % placed.size();
            ops.push_back(Op{Op::Kind::CANCEL, Side::BUY, placed[k].user,
                             placed[k].symbol, 0, 0, placed[k].id});
            continue;
        }

        const UserId   user   = rng() % kUsers;
        const SymbolId symbol = static_cast<SymbolId>(rng() % kSymbols);
        const Side     side   = (rng() & 1) ? Side::BUY : Side::SELL;
        const Qty      qty    = 1 + rng() % 200;
        const Price    px     = kMid + (static_cast<Price>(rng() % 41) - 20) * 5;

        const bool isLimit = roll < 80;                           // 70% limit, 20% market
        ops.push_back(Op{isLimit ? Op::Kind::LIMIT : Op::Kind::MARKET,
                         side, user, symbol, isLimit ? px : 0, qty, 0});
        placed.push_back({user, symbol, makeOrderId(symbol, ++seq[symbol])});   // the Exchange numbers every submit 1, 2, 3...
    }
    return ops;
}
// Deep-book workload: most orders rest far from the mid price, so the books
// grow large and cancels hit real resting orders.
std::vector<Op> generateDeepOps(std::size_t n, unsigned seed) {
    struct Resting { UserId user; SymbolId symbol; OrderId id; };
    std::mt19937 rng(seed);
    std::vector<Op> ops;
    std::vector<Resting> limits;         // only orders that can rest
    ops.reserve(n);
    limits.reserve(n);
std::vector<std::uint64_t> seq(kSymbols, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned roll = rng() % 100;

        if (roll < 10 && !limits.empty()) {                       // 10% cancel
            const Resting& r = limits[rng() % limits.size()];
            ops.push_back(Op{Op::Kind::CANCEL, Side::BUY, r.user, r.symbol, 0, 0, r.id});
            continue;
        }

        const UserId   user   = rng() % kUsers;
        const SymbolId symbol = static_cast<SymbolId>(rng() % kSymbols);
        const Side     side   = (rng() & 1) ? Side::BUY : Side::SELL;
        const Price    away   = (1 + static_cast<Price>(rng() % 1000)) * 5;  // 5..5000 paise
        const OrderId nextId = makeOrderId(symbol, ++seq[symbol]);

        if (roll < 25) {                                          // 15% market, small
            ops.push_back(Op{Op::Kind::MARKET, side, user, symbol, 0,
                             static_cast<Qty>(1 + rng() % 50), 0});
        } else if (roll < 30) {                                   // 5% aggressive limit, small
            const Price px = (side == Side::BUY) ? kMid + away : kMid - away;
            ops.push_back(Op{Op::Kind::LIMIT, side, user, symbol, px,
                             static_cast<Qty>(1 + rng() % 50), 0});
            limits.push_back({user, symbol, nextId});
        } else {                                                  // 70% passive limit
            const Price px = (side == Side::BUY) ? kMid - away : kMid + away;
            ops.push_back(Op{Op::Kind::LIMIT, side, user, symbol, px,
                             static_cast<Qty>(1 + rng() % 200), 0});
            limits.push_back({user, symbol, nextId});
        }
    }
    return ops;
}
// Rich users so almost nothing is rejected for lack of funds.
void seed(Exchange& ex) {
    for (unsigned u = 0; u < kUsers; ++u) {
        const UserId id = ex.accounts().createAccount(toPaise(100'000'000));
        for (SymbolId s = 0; s < kSymbols; ++s)
            ex.accounts().grantShares(id, s, 1'000'000, toPaise(500));
    }
}

void consume(const ExchangeResult& r, Totals& t) {
    t.trades += r.trades.size();
    for (const Trade& tr : r.trades) t.sharesTraded += static_cast<std::uint64_t>(tr.qty);
    if (r.reason != RejectReason::NONE) ++t.rejects;
}

inline void apply(Exchange& ex, const Op& op, Totals& t) {
    switch (op.kind) {
        case Op::Kind::LIMIT:
            consume(ex.submit(op.user, op.symbol, op.side, OrderType::LIMIT, op.price, op.qty), t);
            break;
        case Op::Kind::MARKET:
            consume(ex.submit(op.user, op.symbol, op.side, OrderType::MARKET, 0, op.qty), t);
            break;
        case Op::Kind::CANCEL:
            if (ex.cancel(op.user, op.symbol, op.cancelId)) ++t.cancelsOk;
            break;
    }
}

// Run 1: time the whole batch once (no clock reads inside the loop).
double runThroughput(const std::vector<Op>& ops, Totals& t) {
    Exchange ex(kSymbols);
    seed(ex);
    const auto start = Clock::now();
    for (const Op& op : ops) apply(ex, op, t);
    const auto end = Clock::now();
    return std::chrono::duration<double>(end - start).count();
}

// Run 2: time every single operation.
std::vector<std::uint32_t> runLatency(const std::vector<Op>& ops, Totals& t) {
    Exchange ex(kSymbols);
    seed(ex);
    std::vector<std::uint32_t> lat;
    lat.reserve(ops.size());
    for (const Op& op : ops) {
        const auto a = Clock::now();
        apply(ex, op, t);
        const auto b = Clock::now();
        lat.push_back(static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    }
    return lat;
}

// How much does reading the clock itself cost?
std::uint32_t timerOverheadNs() {
    constexpr int kN = 1'000'000;
    std::uint64_t total = 0;
    for (int i = 0; i < kN; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        total += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }
    return static_cast<std::uint32_t>(total / kN);
}

std::uint32_t percentile(const std::vector<std::uint32_t>& sorted, double p) {
    return sorted[static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1))];
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 1'000'000;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);

#ifndef NDEBUG
    std::puts("WARNING: asserts are ON (not a Release build). These numbers are not valid.");
#endif

    std::printf("Engine benchmark (V1: std::map + std::list)\n");
    const bool deep = (argc > 2 && std::string(argv[2]) == "deep");
    if (deep)
        std::printf("Workload: DEEP BOOK, %zu ops = 70%% passive limit, 5%% aggressive limit, "
                    "15%% market, 10%% cancel; %zu symbols, %u users, seed 42\n\n",
                    n, kSymbols, kUsers);
    else
        std::printf("Workload: %zu ops = 70%% limit, 20%% market, 10%% cancel; "
                    "%zu symbols, %u users, seed 42\n\n", n, kSymbols, kUsers);

    const std::vector<Op> ops = deep ? generateDeepOps(n, 42) : generateOps(n, 42);
    Totals warm;
    runThroughput(ops, warm);                         // warm-up, result discarded

    Totals t;
    const double secs = runThroughput(ops, t);

    Totals t2;
    std::vector<std::uint32_t> lat = runLatency(ops, t2);
    std::sort(lat.begin(), lat.end());
    const std::uint32_t overhead = timerOverheadNs();

    std::printf("Throughput : %.0f ops/sec  (%.3f s total)\n",
                static_cast<double>(n) / secs, secs);
    std::printf("Latency ns : p50 %u | p90 %u | p99 %u | p99.9 %u | max %u\n",
                percentile(lat, 50), percentile(lat, 90), percentile(lat, 99),
                percentile(lat, 99.9), lat.back());
    std::printf("             (each figure includes ~%u ns of clock-reading overhead)\n\n", overhead);

    std::printf("Fingerprint (V2 must match these exactly):\n");
    std::printf("  trades        : %llu\n", static_cast<unsigned long long>(t.trades));
    std::printf("  shares traded : %llu\n", static_cast<unsigned long long>(t.sharesTraded));
    std::printf("  rejects       : %llu\n", static_cast<unsigned long long>(t.rejects));
    std::printf("  cancels ok    : %llu\n", static_cast<unsigned long long>(t.cancelsOk));
    std::printf("  deterministic : %s\n", (t == t2) ? "yes (both runs identical)" : "NO - BUG");
    return 0;
}