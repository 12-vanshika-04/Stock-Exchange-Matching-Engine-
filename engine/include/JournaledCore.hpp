#pragma once
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include "EngineCore.hpp"
#include "Journal.hpp"

namespace exch {

struct RecoveryResult {
    JournalReadResult read;
    bool              diverged = false;   // the journal asked for something replay could not reproduce
    std::string       detail;
    std::uint64_t     journalId = 0;   // this journal's identity; 0 = an older journal that had none
};

// Rebuilds `core` (which must be freshly constructed and empty) from the journal.


inline void truncateJournal(const std::string& path, std::uint64_t validBytes) {
    if (::truncate(path.c_str(), static_cast<off_t>(validBytes)) != 0)
        throw std::runtime_error("cannot repair journal " + path + ": " + std::strerror(errno));
}
inline RecoveryResult replayJournal(const std::string& path, EngineCore& core) {
    RecoveryResult out;
    auto discard = [](const Event&) {};              // replay rebuilds state; nobody needs the events
    const Exchange& ex = core.exchange();
    std::size_t priceRecords = 0;                    // reference-price records seen so far
    bool sawOther = false;                           // any record that is not a reference price
    auto fail = [&](const std::string& why) {
        out.diverged = true;
        out.detail   = why;
    };

    out.read = readJournal(path, [&](const JournalRecord& r) {
        if (out.diverged) return;
        AccountManager& acc = core.exchange().accounts();

        if (r.kind == RecordKind::JOURNAL_ID) {
            if (out.journalId != 0) { fail("the journal holds more than one journal id"); return; }
            if (r.orderId == 0 || r.orderId >= (std::uint64_t{1} << 52)) { fail("the journal's id record is invalid"); return; }
            out.journalId = r.orderId;
            return;
        }

        if (r.kind == RecordKind::REFERENCE_PRICE) {
            if (sawOther) { fail("a reference-price record appears after other records"); return; }
            if (r.symbol >= ex.numSymbols()) {
                fail("the journal has a reference price for symbol " + std::to_string(r.symbol) +
                     " but this run has only " + std::to_string(ex.numSymbols()) + " symbols");
                return;
            }
            if (r.symbol != priceRecords) { fail("the journal's reference prices are not in symbol order"); return; }
            if (r.price != ex.referencePrice(r.symbol)) {
                fail("the journal was written with reference price " + std::to_string(r.price) +
                     " paise for symbol " + std::to_string(r.symbol) +
                     " but this run is configured with " + std::to_string(ex.referencePrice(r.symbol)));
                return;
            }
            ++priceRecords;
            return;
        }

        sawOther = true;
        switch (r.kind) {
            case RecordKind::CREATE_ACCOUNT: {
                const UserId id = acc.createAccount(r.price);
                if (id != r.user)
                    fail("journal expects account id " + std::to_string(r.user) +
                         " but replay created " + std::to_string(id));
                break;
            }
            case RecordKind::GRANT_SHARES:
                if (!acc.exists(r.user) || r.symbol >= acc.account(r.user).positions.size()) {
                    fail("journal grants shares to an unknown user or symbol");
                    break;
                }
                acc.grantShares(r.user, r.symbol, r.qty, r.price);
                break;
            case RecordKind::COMMAND:
                core.process(r.toCommand(), discard);
                break;
            case RecordKind::REFERENCE_PRICE:
            case RecordKind::JOURNAL_ID:
                break;                               // handled above
        }
    });

    if (!out.diverged && out.read.records > 0) {
        if (priceRecords == 0) {
            // A journal from before reference prices existed: every symbol was Rs 500.
            for (SymbolId s = 0; s < ex.numSymbols(); ++s) {
                if (ex.referencePrice(s) != toPaise(500)) {
                    fail("this journal predates reference prices (it was written with Rs 500 for every "
                         "symbol) but this run uses different prices");
                    break;
                }
            }
        } else if (priceRecords != ex.numSymbols()) {
            fail("the journal holds reference prices for " + std::to_string(priceRecords) +
                 " symbols but this run has " + std::to_string(ex.numSymbols()));
        }
    }
    return out;
}
// A random identity for a new journal: 52 bits (so it travels as an exact JavaScript number),
// never 0. Every fresh journal gets its own, so rows saved to a database can tell engine runs apart.
inline std::uint64_t newJournalId() {
    std::random_device rd;
    for (;;) {
        const std::uint64_t id =
            ((static_cast<std::uint64_t>(rd()) << 32) | static_cast<std::uint64_t>(rd())) &
            ((std::uint64_t{1} << 52) - 1);
        if (id != 0) return id;
    }
}

// An EngineCore whose every input is journaled first. The constructor RECOVERS from an
// existing journal: replay, cut off a torn tail, then keep appending.
// It refuses to start (throws) if the journal is corrupt, foreign, or does not replay cleanly.
class JournaledCore {
public:
            JournaledCore(std::size_t numSymbols, const std::string& path,
                  const std::vector<Price>& referencePrices = {})
        : core_(numSymbols, referencePrices) {
        recovery_ = replayJournal(path, core_);
        switch (recovery_.read.status) {
            case JournalStatus::CLEAN:
                break;
            case JournalStatus::TORN_TAIL:
                truncateJournal(path, recovery_.read.validBytes);
                break;
            case JournalStatus::CORRUPT:
                throw std::runtime_error("journal is corrupt in the middle (refusing to guess): " + path);
            case JournalStatus::BAD_HEADER:
                throw std::runtime_error("not an exchange journal: " + path);
        }
        if (recovery_.diverged)
            throw std::runtime_error("journal replay diverged: " + recovery_.detail);
        writer_ = std::make_unique<JournalWriter>(path);
        journalId_ = recovery_.journalId;
        if (recovery_.read.records == 0) {
            // A brand-new journal starts with its identity, then the reference prices it was created with.
            journalId_ = newJournalId();
            writer_->append(JournalRecord::forJournalId(journalId_));
            for (std::size_t i = 0; i < numSymbols; ++i) {
                const SymbolId s = static_cast<SymbolId>(i);
                writer_->append(JournalRecord::forReferencePrice(s, core_.exchange().referencePrice(s)));
            }
            writer_->flush();
        } else if (journalId_ == 0) {
            // An older journal that never had an identity: give it one now, written after what is already there.
            journalId_ = newJournalId();
            writer_->append(JournalRecord::forJournalId(journalId_));
            writer_->flush();
        }
        core_.setEngineId(journalId_);
    }

    UserId createAccount(Price cash) {
        const UserId id = core_.exchange().accounts().createAccount(cash);
        writer_->append(JournalRecord::forCreateAccount(id, cash));
        return id;
    }

    void grantShares(UserId user, SymbolId symbol, Qty qty, Price costPerShare) {
        const AccountManager& acc = core_.exchange().accounts();
        if (!acc.exists(user) || symbol >= acc.account(user).positions.size())
            throw std::invalid_argument("grantShares: unknown user or symbol");
        core_.exchange().accounts().grantShares(user, symbol, qty, costPerShare);
        writer_->append(JournalRecord::forGrantShares(user, symbol, qty, costPerShare));
    }

    // Write-ahead: the command is journaled BEFORE it is applied.
    template <typename Sink>
    void process(const Command& c, Sink&& emit) {
                if (!isQuery(c.type))                                    // queries change nothing: nothing to replay
            writer_->append(JournalRecord::forCommand(c));
        core_.process(c, emit);
    }

    void flush() { writer_->flush(); }
    void sync()  { writer_->sync(); }

    EngineCore&       core()       { return core_; }
    const EngineCore& core() const { return core_; }
    const RecoveryResult& recovery() const { return recovery_; }
    std::uint64_t journalId() const { return journalId_; }

private:
    EngineCore                     core_;
    RecoveryResult                 recovery_;
    std::uint64_t                  journalId_ = 0;
    std::unique_ptr<JournalWriter> writer_;
};

}  // namespace exch