#include <signal.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>
#include "JournaledPipeline.hpp"
#include "TcpServer.hpp"

using namespace exch;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
constexpr std::size_t kSymbols = 6;
// Illustrative reference prices in rupees (not market data), in the gateway's symbol order:
// RELIANCE, TCS, INFY, HDFCBANK, ICICIBANK, SBIN. Each symbol trades within +-20% of its own.
constexpr std::int64_t kReferenceRupees[kSymbols] = {2500, 3500, 1500, 1600, 1100, 800};
}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port    = 9000;
    std::string   journal = "engine.journal";
    unsigned      users   = 1000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)         port    = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--journal" && i + 1 < argc) journal = argv[++i];
        else if (a == "--users" && i + 1 < argc)   users   = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else {
            std::fprintf(stderr, "usage: %s [--port N] [--journal FILE] [--users N]\n", argv[0]);
            return 2;
        }
    }

    try {
        std::vector<Price> prices;
        for (const std::int64_t rupees : kReferenceRupees) prices.push_back(toPaise(rupees));

        // Recovers from the journal if it exists, and refuses to start if the journal was
        // written with different reference prices.
        JournaledPipeline pipe(kSymbols, journal, /*syncToDisk=*/false, prices);

        // Demo seeding: replaced by account messages later.
        const bool fresh = !pipe.core().core().exchange().accounts().exists(0);
        if (fresh) {
            for (unsigned u = 0; u < users; ++u) {
                const UserId id = pipe.core().createAccount(toPaise(10'000'000));   // Rs 1 crore
                for (SymbolId s = 0; s < kSymbols; ++s)
                    pipe.core().grantShares(id, s, 1000, prices[s]);
            }
        }
        pipe.start();

        TcpServer<JournaledPipeline> server(pipe, port);
        server.start();

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        std::printf("engine listening on 127.0.0.1:%u\n", static_cast<unsigned>(server.port()));
        std::printf("journal: %s (%s, %llu records recovered)\n", journal.c_str(),
                    fresh ? "new, seeded" : "recovered",
                    static_cast<unsigned long long>(pipe.recovery().read.records));
        std::printf("engine id: %llu\n", static_cast<unsigned long long>(pipe.core().journalId()));
        std::printf("reference prices (Rs): ");
        for (const std::int64_t rupees : kReferenceRupees) std::printf("%lld ", static_cast<long long>(rupees));
        std::printf("\nCtrl+C to stop\n");
        std::fflush(stdout);

        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

        server.stop();
        pipe.stop();
        std::printf("\nstopped: %llu connections, %llu commands received, %llu events sent, %llu protocol errors\n",
                    static_cast<unsigned long long>(server.connections()),
                    static_cast<unsigned long long>(server.commandsReceived()),
                    static_cast<unsigned long long>(server.eventsSent()),
                    static_cast<unsigned long long>(server.protocolErrors()));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "engine_server: %s\n", ex.what());
        return 1;
    }
    return 0;
}