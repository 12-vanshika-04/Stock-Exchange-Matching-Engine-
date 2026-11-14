#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "JournaledCore.hpp"
#include "Messages.hpp"
#include "SpscQueue.hpp"
#include "Types.hpp"

namespace exch {

//   producer --[in_]--> ENGINE THREAD --[out_]--> consumer
//
// Like EnginePipeline, but every command is journaled before it is applied, and the
// events it causes are released only after the journal holds the command (group commit).
class JournaledPipeline {
public:
    static constexpr std::size_t kInCap    = 1 << 16;
    static constexpr std::size_t kOutCap   = 1 << 17;
    static constexpr std::size_t kMaxBatch = 256;      // commands handled per journal flush

    // Recovers from an existing journal. Throws std::runtime_error if it is corrupt,
    // foreign, or does not replay cleanly.
        JournaledPipeline(std::size_t numSymbols, const std::string& journalPath,
                      bool syncToDisk = false, const std::vector<Price>& referencePrices = {})
        : core_(numSymbols, journalPath, referencePrices),
          syncToDisk_(syncToDisk),
          in_(std::make_unique<SpscQueue<Command, kInCap>>()),
          out_(std::make_unique<SpscQueue<Event, kOutCap>>()) {
        pending_.reserve(4096);
    }

    ~JournaledPipeline() {
        abort_.store(true, std::memory_order_relaxed);   // never hang on a full output queue
        stop();
    }
    JournaledPipeline(const JournaledPipeline&)            = delete;
    JournaledPipeline& operator=(const JournaledPipeline&) = delete;

    // Only touch the core BEFORE start() (to seed accounts) or AFTER stop() (to inspect).
    JournaledCore&        core()           { return core_; }
    const RecoveryResult& recovery() const { return core_.recovery(); }

    void start() {
        core_.flush();                                   // seed records written before start
        thread_ = std::thread([this] { run(); });
    }

    // Graceful: the engine finishes every command already submitted, then exits.
    void stop() {
        if (!thread_.joinable()) return;
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

    bool trySubmit(const Command& c) { return in_->tryPush(c); }    // one producer thread only
    bool tryPollEvent(Event& e)      { return out_->tryPop(e); }    // one consumer thread only

    // Commands whose events have been released (so they are journaled too).
    std::uint64_t commandsProcessed() const { return processed_.load(std::memory_order_relaxed); }

    // True if a journal write failed and the engine thread stopped.
    bool failed() const { return failed_.load(std::memory_order_acquire); }

private:
    static constexpr unsigned kSpinsBeforeYield = 2000;

    bool emitBlocking(const Event& e) {
        while (!out_->tryPush(e)) {
            if (abort_.load(std::memory_order_relaxed)) return false;
            std::this_thread::yield();
        }
        return true;
    }

    void run() {
        try { loop(); }
        catch (...) { failed_.store(true, std::memory_order_release); }
    }

    void loop() {
        Command       c{};
        unsigned      idle = 0;
        std::uint64_t done = 0;
        auto collect = [this](const Event& e) { pending_.push_back(e); };

        for (;;) {
            if (abort_.load(std::memory_order_relaxed)) return;

            // 1. Take up to kMaxBatch commands. process() journals each BEFORE applying it.
            std::size_t n = 0;
            while (n < kMaxBatch && in_->tryPop(c)) {
                core_.process(c, collect);
                ++n;
            }

            if (n == 0) {
                if (stop_.load(std::memory_order_acquire)) {
                    // Everything submitted before stop() is visible by now: one last look.
                    if (!in_->tryPop(c)) return;
                    core_.process(c, collect);
                    n = 1;
                } else {
                    if (idle < kSpinsBeforeYield) ++idle;
                    else                          std::this_thread::yield();
                    continue;
                }
            }
            idle = 0;

            // 2. Make the whole batch durable, THEN let anyone see its events.
            if (syncToDisk_) core_.sync();
            else             core_.flush();
            for (const Event& e : pending_)
                if (!emitBlocking(e)) return;
            pending_.clear();

            done += n;
            processed_.store(done, std::memory_order_relaxed);
        }
    }

    JournaledCore                               core_;
    bool                                        syncToDisk_;
    std::unique_ptr<SpscQueue<Command, kInCap>> in_;
    std::unique_ptr<SpscQueue<Event, kOutCap>>  out_;
    std::vector<Event>                          pending_;   // this batch's events, held until it is durable
    std::atomic<bool>                           stop_{false};
    std::atomic<bool>                           abort_{false};
    std::atomic<bool>                           failed_{false};
    std::atomic<std::uint64_t>                  processed_{0};
    std::thread                                 thread_;
};

}  // namespace exch