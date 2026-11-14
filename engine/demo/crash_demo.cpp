#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include "JournaledPipeline.hpp"

using namespace exch;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr unsigned    kUsers   = 3;
constexpr std::size_t kSymbols = 1;

// ---- a deterministic command stream: command k depends only on (seed, k) ----

std::uint64_t mix(std::uint64_t x) {                 // splitmix64
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

Command makeCommand(std::uint64_t seed, std::uint64_t k) {
    std::uint64_t r = mix(seed ^ (k * 0xD1B54A32D192ED03ull));
    auto next = [&r]() { r = mix(r); return r; };

    Command c{};
    c.symbol    = 0;
    c.user      = static_cast<UserId>(next() % kUsers);
    c.clientRef = k;                                 // the ack protocol relies on this
    const unsigned roll = static_cast<unsigned>(next() % 100);
    if (roll < 15) {
        c.type    = CommandType::CANCEL_ORDER;
        c.orderId = makeOrderId(0, 1 + next() % (k + 1));
    } else {
        c.type = CommandType::NEW_ORDER;
        c.side = (next() & 1) ? Side::BUY : Side::SELL;
        c.qty  = static_cast<Qty>(1 + next() % 100);
        if (roll < 80) {
            c.orderType = OrderType::LIMIT;
            c.price     = toPaise(490 + static_cast<std::int64_t>(next() % 21));
        } else {
            c.orderType = OrderType::MARKET;
        }
    }
    return c;
}

void seedAccounts(JournaledCore& j) {
    for (unsigned i = 0; i < kUsers; ++i) {
        const UserId u = j.createAccount(toPaise(1'000'000));
        j.grantShares(u, 0, 5000, toPaise(500));
    }
}

void seedPlain(EngineCore& core) {
    for (unsigned i = 0; i < kUsers; ++i) {
        const UserId u = core.exchange().accounts().createAccount(toPaise(1'000'000));
        core.exchange().accounts().grantShares(u, 0, 5000, toPaise(500));
    }
}

struct JournalSummary {
    JournalReadResult read;
    std::uint64_t     commands = 0;
    bool              inOrder  = true;   // the commands are exactly makeCommand(seed, 0..n-1)
};

JournalSummary summarize(const std::string& path, std::uint64_t seed) {
    JournalSummary s;
    s.read = readJournal(path, [&](const JournalRecord& r) {
        if (r.kind != RecordKind::COMMAND) return;
        if (!(r == JournalRecord::forCommand(makeCommand(seed, s.commands)))) s.inOrder = false;
        ++s.commands;
    });
    return s;
}

const char* statusName(JournalStatus s) {
    switch (s) {
        case JournalStatus::CLEAN:      return "clean";
        case JournalStatus::TORN_TAIL:  return "torn tail (repaired on restart)";
        case JournalStatus::CORRUPT:    return "CORRUPT";
        case JournalStatus::BAD_HEADER: return "BAD HEADER";
    }
    return "?";
}

// ---------------------------------------------------------------- worker

int runWorker(const std::string& journal, const std::string& ackPath, std::uint64_t seed) {
    try {
        JournaledPipeline pipe(kSymbols, journal);                   // recovers if a journal exists
        if (pipe.recovery().read.records == 0) seedAccounts(pipe.core());
        std::uint64_t next = summarize(journal, seed).commands;      // resume after the last journaled command
        pipe.start();

        const int fd = ::open(ackPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return 2;
        std::uint64_t acked = 0;                                     // commands whose first event we have seen
        if (::pwrite(fd, &acked, sizeof acked, 0) != static_cast<ssize_t>(sizeof acked)) return 2;
        // The ack file now exists: that tells the driver the seed records are journaled.

        Event e;
        for (;;) {
            for (int i = 0; i < 128; ++i) {
                const Command c = makeCommand(seed, next);
                if (!pipe.trySubmit(c)) break;                       // queue full: try again next loop
                ++next;
            }
            bool moved = false;
            while (pipe.tryPollEvent(e)) {
                if (e.type != EventType::TRADE && e.clientRef + 1 > acked) {
                    acked = e.clientRef + 1;
                    moved = true;
                }
            }
            if (moved && ::pwrite(fd, &acked, sizeof acked, 0) != static_cast<ssize_t>(sizeof acked))
                return 2;
            if (pipe.failed()) return 3;
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "worker: %s\n", ex.what());
        return 2;
    }
}

// ---------------------------------------------------------------- checks

bool sameState(const EngineCore& a, const EngineCore& b, std::string& why) {
    for (UserId u = 0; u < kUsers; ++u) {
        const Account& x = a.exchange().accounts().account(u);
        const Account& y = b.exchange().accounts().account(u);
        if (x.cash != y.cash || x.reservedCash != y.reservedCash) {
            why = "cash or reserved cash differs for user " + std::to_string(u);
            return false;
        }
        for (std::size_t s = 0; s < x.positions.size(); ++s) {
            const Position& p = x.positions[s];
            const Position& q = y.positions[s];
            if (p.qty != q.qty || p.reservedQty != q.reservedQty || p.costBasis != q.costBasis) {
                why = "position differs for user " + std::to_string(u);
                return false;
            }
        }
    }
    const OrderBook& ba = a.exchange().engine().book(0);
    const OrderBook& bb = b.exchange().engine().book(0);
    if (ba.orderCount() != bb.orderCount()) { why = "resting order count differs"; return false; }
    if (ba.depth(Side::BUY, 100000) != bb.depth(Side::BUY, 100000)) { why = "bid side differs"; return false; }
    if (ba.depth(Side::SELL, 100000) != bb.depth(Side::SELL, 100000)) { why = "ask side differs"; return false; }
    return true;
}

// ---------------------------------------------------------------- driver

int runDriver(unsigned rounds, std::uint64_t seed, bool keep) {
    const std::string tag     = std::to_string(::getpid());
    const std::string journal = (fs::temp_directory_path() / ("exch_crashdemo_" + tag + ".journal")).string();
    const std::string ack     = (fs::temp_directory_path() / ("exch_crashdemo_" + tag + ".ack")).string();
    std::error_code ec;
    fs::remove(journal, ec);
    fs::remove(ack, ec);

    std::printf("Crash test: %u rounds, SIGKILL after a random 20-200 ms, seed %llu\n",
                rounds, static_cast<unsigned long long>(seed));
    std::printf("journal: %s\n\n", journal.c_str());
    std::printf("%5s %8s %12s %12s  %s\n", "round", "ran(ms)", "acked", "journaled", "journal state after the kill");

    std::mt19937 rng(static_cast<unsigned>(seed));
    std::uint64_t prevCommands = 0;
    unsigned torn = 0;
    bool ok = true;

    for (unsigned round = 1; round <= rounds && ok; ++round) {
        fs::remove(ack, ec);
        const pid_t pid = ::fork();
        if (pid < 0) { std::perror("fork"); return 1; }
        if (pid == 0) {
            const std::string seedStr = std::to_string(seed);
            ::execl("/proc/self/exe", "engine_crash_demo", "--worker",
                    journal.c_str(), ack.c_str(), seedStr.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        // Wait until the worker has recovered and is running (its ack file appears).
        bool ready = false, diedEarly = false;
        const auto readyDeadline = Clock::now() + std::chrono::seconds(60);
        while (Clock::now() < readyDeadline) {
            if (fs::exists(ack, ec)) { ready = true; break; }
            int st = 0;
            if (::waitpid(pid, &st, WNOHANG) == pid) { diedEarly = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (diedEarly) { std::printf("FAIL: round %u: worker exited before it was ready\n", round); ok = false; break; }
        if (!ready) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            std::printf("FAIL: round %u: worker did not become ready within 60 s\n", round);
            ok = false;
            break;
        }

        const unsigned delayMs = 20 + static_cast<unsigned>(rng() % 181);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        ::kill(pid, SIGKILL);
        int st = 0;
        ::waitpid(pid, &st, 0);
        if (!(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL)) {
            std::printf("FAIL: round %u: worker ended by itself (wait status %d), not by our kill\n", round, st);
            ok = false;
            break;
        }

        std::uint64_t acked = 0;
        {
            const int fd = ::open(ack.c_str(), O_RDONLY);
            if (fd >= 0) {
                if (::pread(fd, &acked, sizeof acked, 0) != static_cast<ssize_t>(sizeof acked)) acked = 0;
                ::close(fd);
            }
        }
        const JournalSummary s = summarize(journal, seed);
        std::printf("%5u %8u %12llu %12llu  %s\n", round, delayMs,
                    static_cast<unsigned long long>(acked),
                    static_cast<unsigned long long>(s.commands), statusName(s.read.status));

        if (s.read.status == JournalStatus::TORN_TAIL) ++torn;
        if (s.read.status == JournalStatus::CORRUPT || s.read.status == JournalStatus::BAD_HEADER) {
            std::printf("FAIL: the journal is damaged\n");
            ok = false;
        }
        if (!s.inOrder) {
            std::printf("FAIL: the journal's commands are not the expected stream (a gap, duplicate or reorder)\n");
            ok = false;
        }
        if (s.commands < acked) {
            std::printf("FAIL: a client was told about command %llu but the journal only holds %llu commands\n",
                        static_cast<unsigned long long>(acked),
                        static_cast<unsigned long long>(s.commands));
            ok = false;
        }
        if (s.commands < prevCommands) {
            std::printf("FAIL: the journal shrank (%llu -> %llu commands)\n",
                        static_cast<unsigned long long>(prevCommands),
                        static_cast<unsigned long long>(s.commands));
            ok = false;
        }
        prevCommands = s.commands;
    }

    // ---- final verification: the recovered engine must equal an engine that never crashed ----
    std::uint64_t total = 0;
    if (ok) {
        try {
            JournaledCore recovered(kSymbols, journal);
            const JournalSummary s = summarize(journal, seed);
            total = s.commands;

            EngineCore reference(kSymbols);
            seedPlain(reference);
            for (std::uint64_t k = 0; k < total; ++k)
                reference.process(makeCommand(seed, k), [](const Event&) {});

            std::string why;
            if (!s.inOrder) { std::printf("FAIL: final journal is not the expected command stream\n"); ok = false; }
            if (s.read.status != JournalStatus::CLEAN) { std::printf("FAIL: final journal is not clean\n"); ok = false; }
            if (!sameState(reference, recovered.core(), why)) {
                std::printf("FAIL: recovered state differs from an engine that never crashed: %s\n", why.c_str());
                ok = false;
            }
            const AccountManager& acc = recovered.core().exchange().accounts();
            if (acc.totalCash() != static_cast<Price>(kUsers) * toPaise(1'000'000) ||
                acc.totalShares(0) != static_cast<Qty>(kUsers) * 5000) {
                std::printf("FAIL: total cash or shares changed\n");
                ok = false;
            }
        } catch (const std::exception& ex) {
            std::printf("FAIL: could not recover from the final journal: %s\n", ex.what());
            ok = false;
        }
    }

    std::printf("\n%s: %llu commands journaled across %u kills (%u of them left a torn tail)\n",
                ok ? "PASS" : "FAILED", static_cast<unsigned long long>(total), rounds, torn);
    if (ok)
        std::printf("Recovered state equals an engine that never crashed. Cash and shares are conserved.\n");

    if (keep) {
        std::printf("kept: %s\n", journal.c_str());
    } else {
        fs::remove(journal, ec);
        fs::remove(ack, ec);
    }
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--worker")
        return runWorker(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10));

    unsigned rounds = 12;
    std::uint64_t seed = 1;
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rounds" && i + 1 < argc)    rounds = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--keep")                 keep = true;
        else {
            std::fprintf(stderr, "usage: %s [--rounds N] [--seed S] [--keep]\n", argv[0]);
            return 2;
        }
    }
    return runDriver(rounds, seed, keep);
}