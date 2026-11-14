#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include "EngineCore.hpp"
#include "Messages.hpp"
#include "SpscQueue.hpp"
#include "Types.hpp"

namespace exch {

//   producer thread --[in_ queue]--> ENGINE THREAD --[out_ queue]--> consumer thread
//
// The engine thread is the only thread that ever touches the EngineCore (books and
// balances). Everything else talks to it through the two queues.
class EnginePipeline {
public:
    static constexpr std::size_t kInCap  = 1 << 16;   // commands waiting for the engine
    static constexpr std::size_t kOutCap = 1 << 17;   // events waiting for the consumer

        explicit EnginePipeline(std::size_t numSymbols, const std::vector<Price>& referencePrices = {})
        : core_(numSymbols, referencePrices),
          in_(std::make_unique<SpscQueue<Command, kInCap>>()),
          out_(std::make_unique<SpscQueue<Event, kOutCap>>()) {}

    ~EnginePipeline() {
        abort_.store(true, std::memory_order_relaxed);   // never hang on a full output queue
        stop();
    }
    EnginePipeline(const EnginePipeline&)            = delete;
    EnginePipeline& operator=(const EnginePipeline&) = delete;

    // Only touch the core BEFORE start() (to seed accounts) or AFTER stop() (to inspect).
    EngineCore& core() { return core_; }

    void start() { thread_ = std::thread([this] { run(); }); }

    // Graceful: the engine finishes every command already submitted, then exits.
    // Call it from the producer side, after the last trySubmit.
    void stop() {
        if (!thread_.joinable()) return;
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

    bool trySubmit(const Command& c) { return in_->tryPush(c); }    // one producer thread only
    bool tryPollEvent(Event& e)      { return out_->tryPop(e); }    // one consumer thread only

    std::uint64_t commandsProcessed() const {
        return processed_.load(std::memory_order_relaxed);
    }

private:
    static constexpr unsigned kSpinsBeforeYield = 2000;

    void emitBlocking(const Event& e) {
        while (!out_->tryPush(e)) {                      // output full: wait for the consumer
            if (abort_.load(std::memory_order_relaxed)) return;
            std::this_thread::yield();
        }
    }

    void run() {
        Command       c{};
        unsigned      idle = 0;
        std::uint64_t done = 0;
        auto sink = [this](const Event& e) { emitBlocking(e); };

        for (;;) {
            if (abort_.load(std::memory_order_relaxed)) return;

            if (!in_->tryPop(c)) {
                if (stop_.load(std::memory_order_acquire)) {
                    // Everything submitted before stop() is visible by now: one last check.
                    if (!in_->tryPop(c)) return;
                } else {
                    if (idle < kSpinsBeforeYield) ++idle;
                    else                          std::this_thread::yield();
                    continue;
                }
            }
            idle = 0;
            core_.process(c, sink);
            processed_.store(++done, std::memory_order_relaxed);   // one writer: no atomic RMW needed
        }
    }

    EngineCore                                  core_;
    std::unique_ptr<SpscQueue<Command, kInCap>> in_;     // the queues are big: keep them on the heap
    std::unique_ptr<SpscQueue<Event, kOutCap>>  out_;
    std::atomic<bool>                           stop_{false};
    std::atomic<bool>                           abort_{false};
    std::atomic<std::uint64_t>                  processed_{0};
    std::thread                                 thread_;
};

}  // namespace exch