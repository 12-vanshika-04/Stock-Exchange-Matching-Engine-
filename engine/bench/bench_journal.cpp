#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include "EnginePipeline.hpp"
#include "JournaledPipeline.hpp"

using namespace exch;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kSymbols = 6;
constexpr unsigned    kUsers   = 1000;
constexpr Price       kMid     = toPaise(500);

// Same workload as the earlier pipeline benchmark (same seed, same random calls).
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

void seedPlain(EngineCore& core) {
    for (unsigned u = 0; u < kUsers; ++u) {
        const UserId id = core.exchange().accounts().createAccount(toPaise(100'000'000));
        for (SymbolId s = 0; s < kSymbols; ++s)
            core.exchange().accounts().grantShares(id, s, 1'000'000, toPaise(500));
    }
}

void seedJournaled(JournaledCore& j) {
    for (unsigned u = 0; u < kUsers; ++u) {
        const UserId id = j.createAccount(toPaise(100'000'000));
        for (SymbolId s = 0; s < kSymbols; ++s) j.grantShares(id, s, 1'000'000, toPaise(500));
    }
}

std::uint64_t countEvents(const std::vector<Command>& cmds) {
    EngineCore core(kSymbols);
    seedPlain(core);
    std::uint64_t n = 0;
    for (const Command& c : cmds) core.process(c, [&n](const Event&) { ++n; });
    return n;
}

// Producer thread -> pipeline -> this thread. Returns seconds, or -1 on timeout.
template <typename Pipe>
double drive(Pipe& pipe, const std::vector<Command>& cmds, std::uint64_t expectedEvents) {
    pipe.start();
    std::atomic<bool> giveUp{false};
    const auto start    = Clock::now();
    const auto deadline = start + std::chrono::seconds(180);

    std::thread producer([&] {
        for (const Command& c : cmds)
            while (!pipe.trySubmit(c)) {
                if (giveUp.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();
            }
    });

    std::uint64_t seen = 0;
    Event e;
    bool timedOut = false;
    while (seen < expectedEvents) {
        if (pipe.tryPollEvent(e)) { ++seen; continue; }
        if (Clock::now() > deadline) { timedOut = true; break; }
        std::this_thread::yield();
    }
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    giveUp.store(true, std::memory_order_relaxed);
    producer.join();
    return timedOut ? -1.0 : secs;
}

double runNone(const std::vector<Command>& cmds, std::uint64_t events) {
    EnginePipeline pipe(kSymbols);
    seedPlain(pipe.core());
    return drive(pipe, cmds, events);
}

double runJournal(const std::vector<Command>& cmds, std::uint64_t events, bool sync,
                  const std::string& path, bool keep = false) {
    std::error_code ec;
    fs::remove(path, ec);
    double secs = -1.0;
    {
        JournaledPipeline pipe(kSymbols, path, sync);
        seedJournaled(pipe.core());
        secs = drive(pipe, cmds, events);
    }
    if (!keep) fs::remove(path, ec);
    return secs;
}

template <typename F>
bool medianSeconds(F run, double& out) {
    std::vector<double> v;
    for (int i = 0; i < 3; ++i) {
        const double s = run();
        if (s < 0) return false;
        v.push_back(s);
    }
    std::sort(v.begin(), v.end());
    out = v[1];
    return true;
}

// What does the disk charge for one batch: write 256 records, then fdatasync?
void measureSyncLatency(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { std::puts("  (could not create a temp file)"); return; }
    std::vector<char> buf(256 * kJournalRecordSize, 'x');
    std::vector<std::uint32_t> us;
    for (int i = 0; i < 300; ++i) {
        const auto a = Clock::now();
        if (::write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) break;
        if (::fdatasync(fd) != 0) break;
        const auto b = Clock::now();
        us.push_back(static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(b - a).count()));
    }
    ::close(fd);
    fs::remove(path, ec);
    if (us.empty()) { std::puts("  (write or fdatasync failed)"); return; }
    std::sort(us.begin(), us.end());
    const std::uint32_t p50 = us[us.size() / 2];
    std::printf("  one batch (%zu bytes) + fdatasync, %zu samples: p50 %u us | p99 %u us | max %u us\n",
                buf.size(), us.size(), p50, us[us.size() * 99 / 100], us.back());
    std::printf("  so a sync per 256-command batch caps throughput near %.0f commands/sec\n",
                256.0 / (static_cast<double>(p50) / 1e6));
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 1'000'000;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);
    const std::size_t nSync = std::min<std::size_t>(n, 100'000);

#ifndef NDEBUG
    std::puts("WARNING: asserts are ON (not a Release build). These numbers are not valid.");
#endif
    std::printf("Durability cost: %zu commands (70%% limit, 20%% market, 10%% cancel), %zu symbols, "
                "%u users, seed 42\n", n, kSymbols, kUsers);
    std::printf("Hardware threads: %u. The pipeline runs 3 busy threads: producer, engine, consumer.\n\n",
                std::thread::hardware_concurrency());
    std::fflush(stdout);

    const std::vector<Command> cmds = generateCommands(n, 42);
    const std::vector<Command> prefix(cmds.begin(), cmds.begin() + static_cast<std::ptrdiff_t>(nSync));
    const std::uint64_t evAll  = countEvents(cmds);
    const std::uint64_t evSync = countEvents(prefix);

    const std::string journal =
        (fs::temp_directory_path() / ("exch_bench_" + std::to_string(::getpid()) + ".journal")).string();

    double sNone = 0, sFlush = 0, sSync = 0;
    const bool okNone  = medianSeconds([&] { return runNone(cmds, evAll); }, sNone);
    const bool okFlush = medianSeconds([&] { return runJournal(cmds, evAll, false, journal); }, sFlush);
    const bool okSync  = medianSeconds([&] { return runJournal(prefix, evSync, true, journal); }, sSync);

    const double opsNone  = okNone  ? static_cast<double>(n) / sNone : 0;
    const double opsFlush = okFlush ? static_cast<double>(n) / sFlush : 0;
    const double opsSync  = okSync  ? static_cast<double>(nSync) / sSync : 0;

    std::puts("Throughput, same threaded engine, median of 3 runs:");
    if (okNone)  std::printf("  no journal                           : %10.0f commands/sec  (%zu commands)\n", opsNone, n);
    else         std::puts("  no journal                           : TIMED OUT");
    if (okFlush) std::printf("  journal, flush per batch             : %10.0f commands/sec  (%3.0f%% of no journal)  survives a process crash\n",
                             opsFlush, okNone ? 100.0 * opsFlush / opsNone : 0.0);
    else         std::puts("  journal, flush per batch             : TIMED OUT");
    if (okSync)  std::printf("  journal, flush + fdatasync per batch : %10.0f commands/sec  (%3.0f%% of no journal)  survives power loss  (%zu commands)\n",
                             opsSync, okNone ? 100.0 * opsSync / opsNone : 0.0, nSync);
    else         std::puts("  journal, flush + fdatasync per batch : TIMED OUT");
    std::printf("  (a batch is at most %zu commands; the journal costs %zu bytes per command)\n\n",
                JournaledPipeline::kMaxBatch, kJournalRecordSize);
    std::fflush(stdout);

    std::puts("Raw disk cost of the sync:");
    measureSyncLatency(journal);
    std::puts("");
    std::fflush(stdout);

    std::puts("Recovery speed (replay the whole journal into a fresh engine):");
    if (runJournal(cmds, evAll, false, journal, /*keep=*/true) < 0) {
        std::puts("  could not build the journal: TIMED OUT");
    } else {
        const double mb = static_cast<double>(fs::file_size(journal)) / 1e6;
        const auto t0 = Clock::now();
        JournaledCore recovered(kSymbols, journal);
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        const double records = static_cast<double>(recovered.recovery().read.records);
        std::printf("  journal of %.0f records (%.1f MB) replayed in %.3f s = %.0f records/sec\n",
                    records, mb, secs, records / secs);
        std::puts("  (the file was still in the OS cache; after a reboot the read would be slower)");
    }
    std::error_code ec;
    fs::remove(journal, ec);
    return (okNone && okFlush && okSync) ? 0 : 1;
}