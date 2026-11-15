#include <gtest/gtest.h>
#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "JournaledCore.hpp"

using namespace exch;
namespace fs = std::filesystem;

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

JournalRecord rec(std::uint64_t i) {
    JournalRecord r;
    r.kind      = RecordKind::COMMAND;
    r.cmdType   = CommandType::NEW_ORDER;
    r.side      = (i & 1) ? Side::SELL : Side::BUY;
    r.orderType = OrderType::LIMIT;
    r.symbol    = static_cast<SymbolId>(i % 6);
    r.user      = static_cast<UserId>(i);
    r.price     = 50000 + static_cast<Price>(i);
    r.qty       = static_cast<Qty>(i + 1);
    r.orderId   = i * 3;
    r.clientRef = 1000 + i;
    return r;
}

void writeRecords(const std::string& path, std::uint64_t n) {
    JournalWriter w(path);
    for (std::uint64_t i = 0; i < n; ++i) w.append(rec(i));
}

JournalReadResult readAll(const std::string& path, std::vector<JournalRecord>& out) {
    return readJournal(path, [&](const JournalRecord& r) { out.push_back(r); });
}

std::uint64_t recordOffset(std::uint64_t index) {
    return kJournalHeaderSize + index * kJournalRecordSize;
}

void flipByte(const std::string& path, std::uint64_t offset) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.get(c);
    c = static_cast<char>(c ^ 0x5A);
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(c);
}

}  // namespace

// ---------- CRC and record encoding ----------

TEST(Crc32, MatchesTheStandardCheckValue) {
    const char* text = "123456789";
    EXPECT_EQ(crc32(reinterpret_cast<const std::uint8_t*>(text), 9), 0xCBF43926u);
}

TEST(JournalCodec, RoundTripsEveryField) {
    JournalRecord in;
    in.kind      = RecordKind::COMMAND;
    in.cmdType   = CommandType::CANCEL_ORDER;
    in.side      = Side::SELL;
    in.orderType = OrderType::MARKET;
    in.symbol    = 5;
    in.user      = 123456;
    in.price     = -42;
    in.qty       = 9'876'543'210LL;
    in.orderId   = makeOrderId(3, 77);
    in.clientRef = 0xDEADBEEFCAFEULL;

    std::uint8_t buf[kJournalPayloadSize];
    encodePayload(in, buf);
    JournalRecord out;
    ASSERT_TRUE(decodePayload(buf, out));
    EXPECT_TRUE(in == out);
}

TEST(JournalCodec, RejectsAnUnknownKind) {
    std::uint8_t buf[kJournalPayloadSize] = {};      // kind 0 is not valid
    JournalRecord out;
    EXPECT_FALSE(decodePayload(buf, out));
}

// ---------- the file: write, read, damage ----------

TEST(Journal, RoundTrip) {
    TempFile f("roundtrip");
    writeRecords(f.path(), 5);
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::CLEAN);
    EXPECT_EQ(r.records, 5u);
    EXPECT_EQ(r.validBytes, kJournalHeaderSize + 5 * kJournalRecordSize);
    EXPECT_EQ(fs::file_size(f.path()), r.validBytes);
    ASSERT_EQ(got.size(), 5u);
    for (std::uint64_t i = 0; i < 5; ++i) EXPECT_TRUE(got[i] == rec(i)) << "record " << i;
}

TEST(Journal, MissingFileReadsAsEmpty) {
    TempFile f("missing");                           // never created
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::CLEAN);
    EXPECT_EQ(r.records, 0u);
    EXPECT_EQ(r.validBytes, 0u);
}

TEST(Journal, HeaderOnlyIsEmptyAndClean) {
    TempFile f("headeronly");
    { JournalWriter w(f.path()); }
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::CLEAN);
    EXPECT_EQ(r.records, 0u);
    EXPECT_EQ(r.validBytes, kJournalHeaderSize);
}

TEST(Journal, TruncatedLastRecordIsATornTail) {
    TempFile f("truncated");
    writeRecords(f.path(), 5);
    fs::resize_file(f.path(), fs::file_size(f.path()) - 10);     // crash mid-write of record 5
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::TORN_TAIL);
    EXPECT_EQ(r.records, 4u);
    EXPECT_EQ(r.validBytes, recordOffset(4));                    // cut the file here to repair it
    ASSERT_EQ(got.size(), 4u);
    EXPECT_TRUE(got[3] == rec(3));
}

TEST(Journal, CorruptLastRecordIsATornTail) {
    TempFile f("corruptlast");
    writeRecords(f.path(), 5);
    flipByte(f.path(), recordOffset(4) + 10);
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::TORN_TAIL);
    EXPECT_EQ(r.records, 4u);
}

TEST(Journal, CorruptMiddleRecordIsCorruption) {
    TempFile f("corruptmiddle");
    writeRecords(f.path(), 5);
    flipByte(f.path(), recordOffset(2) + 10);
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::CORRUPT);
    EXPECT_EQ(r.records, 2u);                                    // only what came before the damage
}

TEST(Journal, ForeignFileIsRejected) {
    TempFile f("foreign");
    { std::ofstream o(f.path(), std::ios::binary); o << "this is not a journal file at all"; }
    std::vector<JournalRecord> got;
    EXPECT_EQ(readAll(f.path(), got).status, JournalStatus::BAD_HEADER);
}

TEST(Journal, PartialHeaderIsATornTail) {
    TempFile f("partialheader");
    { std::ofstream o(f.path(), std::ios::binary); o << "EXJ"; }              // start of our magic
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::TORN_TAIL);
    EXPECT_EQ(r.validBytes, 0u);

    { std::ofstream o(f.path(), std::ios::binary | std::ios::trunc); o << "XYZ"; }   // not our magic
    EXPECT_EQ(readAll(f.path(), got).status, JournalStatus::BAD_HEADER);
}

TEST(Journal, ReopeningAppends) {
    TempFile f("reopen");
    { JournalWriter w(f.path()); for (std::uint64_t i = 0; i < 3; ++i) w.append(rec(i)); }
    { JournalWriter w(f.path()); for (std::uint64_t i = 3; i < 5; ++i) w.append(rec(i)); }
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::CLEAN);
    ASSERT_EQ(got.size(), 5u);
    EXPECT_TRUE(got[4] == rec(4));
}

TEST(Journal, WriterRefusesAForeignFile) {
    TempFile f("writerforeign");
    { std::ofstream o(f.path(), std::ios::binary); o << "this is not a journal file at all"; }
    EXPECT_THROW({ JournalWriter w(f.path()); }, std::runtime_error);
}

TEST(Journal, ManyRecordsCrossTheBufferLimit) {
    TempFile f("many");
    writeRecords(f.path(), 5000);                    // about 230 KB: several automatic flushes
    std::vector<JournalRecord> got;
    const JournalReadResult r = readAll(f.path(), got);
    EXPECT_EQ(r.status, JournalStatus::CLEAN);
    ASSERT_EQ(got.size(), 5000u);
    EXPECT_TRUE(got.front() == rec(0));
    EXPECT_TRUE(got.back() == rec(4999));
}

// ---------- recovery: the journal rebuilds the engine ----------

namespace {

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

}  // namespace

TEST(JournaledCore, RecoveryReproducesTheExactState) {
    TempFile file("recover");
    const std::vector<Command> first = randomCommands(11, 5000, 3);
    const std::vector<Command> more  = randomCommands(12, 2000, 3);

    EngineCore ref(1);                               // the same history, with no journal involved
    seedPlain(ref);
    for (const Command& c : first) ref.process(c, [](const Event&) {});

    {
        JournaledCore live(1, file.path());
        seedJournaled(live);
        for (const Command& c : first) live.process(c, [](const Event&) {});
        live.flush();
    }                                                // the process "dies" here

    JournaledCore recovered(1, file.path());
    EXPECT_EQ(recovered.recovery().read.status, JournalStatus::CLEAN);
    EXPECT_EQ(recovered.recovery().read.records, 8u + first.size());   // 1 id + 1 price + 3 accounts + 3 grants + commands
    expectSameState(ref, recovered.core(), 3);

    // The strongest check: from here on, both engines behave identically.
    std::vector<Event> a, b;
    for (const Command& c : more) {
        ref.process(c, [&](const Event& e) { a.push_back(e); });
        recovered.process(c, [&](const Event& e) { b.push_back(e); });
    }
    ASSERT_EQ(a.size(), b.size());
    std::size_t trades = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_TRUE(sameEvent(a[i], b[i])) << "streams diverge at event " << i;
        if (a[i].type == EventType::TRADE) ++trades;
    }
    EXPECT_GT(trades, 0u);                           // the test is not vacuous
}

TEST(JournaledCore, TornTailIsDroppedAndTheJournalKeepsWorking) {
    TempFile file("torn");
    const std::vector<Command> cmds = randomCommands(21, 3000, 3);
    {
        JournaledCore live(1, file.path());
        seedJournaled(live);
        for (const Command& c : cmds) live.process(c, [](const Event&) {});
        live.flush();
    }
    fs::resize_file(file.path(), fs::file_size(file.path()) - 10);   // the last record is cut short

    EngineCore ref(1);                               // saw everything except the last command
    seedPlain(ref);
    for (std::size_t i = 0; i + 1 < cmds.size(); ++i) ref.process(cmds[i], [](const Event&) {});

    {
        JournaledCore recovered(1, file.path());
        EXPECT_EQ(recovered.recovery().read.status, JournalStatus::TORN_TAIL);
        EXPECT_EQ(recovered.recovery().read.records, 8u + cmds.size() - 1);
        expectSameState(ref, recovered.core(), 3);
        EXPECT_EQ(fs::file_size(file.path()), recovered.recovery().read.validBytes);   // repaired

        recovered.process(cmds.back(), [](const Event&) {});         // the lost command is resubmitted
        recovered.flush();
    }
    JournaledCore again(1, file.path());             // a second restart finds a clean, complete journal
    EXPECT_EQ(again.recovery().read.status, JournalStatus::CLEAN);
    EXPECT_EQ(again.recovery().read.records, 8u + cmds.size());
}

TEST(JournaledCore, CorruptJournalRefusesToStart) {
    TempFile file("corruptstart");
    {
        JournaledCore live(1, file.path());
        seedJournaled(live);
        live.flush();
    }
    flipByte(file.path(), recordOffset(2) + 10);     // damage in the middle of the file
    EXPECT_THROW(JournaledCore(1, file.path()), std::runtime_error);
}

TEST(JournaledCore, DivergentJournalRefusesToStart) {
    TempFile file("diverge");
    {
        JournalWriter w(file.path());
        w.append(JournalRecord::forCreateAccount(5, toPaise(100)));   // but replay would create account 0
    }
    EXPECT_THROW(JournaledCore(1, file.path()), std::runtime_error);
}
// ---------- the journal records the prices it was created with ----------

namespace {

Command limitCmd(UserId u, Side s, Price px, Qty q, std::uint64_t ref) {
    Command c{};
    c.type      = CommandType::NEW_ORDER;
    c.side      = s;
    c.orderType = OrderType::LIMIT;
    c.symbol    = 0;
    c.user      = u;
    c.price     = px;
    c.qty       = q;
    c.clientRef = ref;
    return c;
}

std::vector<JournalRecord> allRecords(const std::string& path) {
    std::vector<JournalRecord> out;
    readJournal(path, [&](const JournalRecord& r) { out.push_back(r); });
    return out;
}

}  // namespace

TEST(JournalConfig, AFreshJournalStartsWithTheReferencePrices) {
    TempFile file("cfg_fresh");
    { JournaledCore j(2, file.path(), {toPaise(2500), toPaise(800)}); }
    const auto recs = allRecords(file.path());
    ASSERT_EQ(recs.size(), 3u);                                  // the journal's id, then one price per symbol
    EXPECT_EQ(recs[0].kind, RecordKind::JOURNAL_ID);
    EXPECT_EQ(recs[1].kind, RecordKind::REFERENCE_PRICE);
    EXPECT_EQ(recs[1].symbol, 0);
    EXPECT_EQ(recs[1].price, toPaise(2500));
    EXPECT_EQ(recs[2].kind, RecordKind::REFERENCE_PRICE);
    EXPECT_EQ(recs[2].symbol, 1);
    EXPECT_EQ(recs[2].price, toPaise(800));
}

TEST(JournalConfig, RestartingWithTheSamePricesWorks) {
    TempFile file("cfg_same");
    { JournaledCore j(2, file.path(), {toPaise(2500), toPaise(800)}); }
    JournaledCore again(2, file.path(), {toPaise(2500), toPaise(800)});
    EXPECT_EQ(again.recovery().read.status, JournalStatus::CLEAN);
    EXPECT_EQ(again.recovery().read.records, 3u);
    EXPECT_EQ(allRecords(file.path()).size(), 3u);               // no second copy of the id or the prices
}

TEST(JournalConfig, RestartingWithDifferentPricesIsRefused) {
    TempFile file("cfg_diff");
    { JournaledCore j(2, file.path(), {toPaise(2500), toPaise(800)}); }
    EXPECT_THROW(JournaledCore(2, file.path(), {toPaise(2500), toPaise(900)}), std::runtime_error);   // another price
    EXPECT_THROW(JournaledCore(2, file.path()), std::runtime_error);                                  // the defaults
    EXPECT_THROW(JournaledCore(3, file.path()), std::runtime_error);                                  // another symbol count
    EXPECT_THROW(JournaledCore(1, file.path(), {toPaise(2500)}), std::runtime_error);                 // fewer symbols
}

TEST(JournalConfig, AJournalFromBeforeReferencePricesIsAcceptedOnlyAtTheDefaults) {
    TempFile file("cfg_legacy");
    {
        JournalWriter w(file.path());                            // no price records: the old format
        w.append(JournalRecord::forCreateAccount(0, toPaise(1000)));
        w.append(JournalRecord::forGrantShares(0, 0, 50, toPaise(500)));
    }
    {
        JournaledCore ok(1, file.path());                        // the default is Rs 500: fine
        EXPECT_EQ(ok.core().exchange().accounts().account(0).cash, toPaise(1000));
        EXPECT_EQ(ok.core().exchange().accounts().account(0).positions[0].qty, 50);
    }
    EXPECT_THROW(JournaledCore(1, file.path(), {toPaise(2500)}), std::runtime_error);
}

TEST(JournalConfig, CustomPricesSurviveARestart) {
    TempFile file("cfg_custom");
    const std::vector<Price> prices = {toPaise(2500)};
    Price buyerCash = 0;
    {
        JournaledCore live(1, file.path(), prices);
        const UserId seller = live.createAccount(toPaise(1'000'000));
        const UserId buyer  = live.createAccount(toPaise(1'000'000));
        live.grantShares(seller, 0, 100, toPaise(2400));
        auto discard = [](const Event&) {};
        live.process(limitCmd(seller, Side::SELL, toPaise(2500), 10, 1), discard);
        live.process(limitCmd(buyer, Side::BUY, toPaise(2500), 10, 2), discard);
        buyerCash = live.core().exchange().accounts().account(buyer).cash;
        EXPECT_EQ(buyerCash, toPaise(1'000'000) - toPaise(2500) * 10);       // the trade happened
        live.flush();
    }
    JournaledCore recovered(1, file.path(), prices);
    EXPECT_EQ(recovered.recovery().read.status, JournalStatus::CLEAN);
    const Account& b = recovered.core().exchange().accounts().account(1);
    EXPECT_EQ(b.cash, buyerCash);                                // replayed under the Rs 2,500 band, not the default
    EXPECT_EQ(b.positions[0].qty, 10);
}
TEST(JournalQueries, QueriesAreAnsweredButNeverJournaled) {
    TempFile file("queries");
    {
        JournaledCore j(1, file.path());
        const UserId u = j.createAccount(toPaise(100000));
        auto discard = [](const Event&) {};
        j.process(limitCmd(u, Side::BUY, toPaise(500), 10, 1), discard);

        std::size_t events = 0;
        Command qa{};
        qa.type      = CommandType::QUERY_ACCOUNT;
        qa.user      = u;
        qa.clientRef = 2;
        j.process(qa, [&](const Event&) { ++events; });
        EXPECT_EQ(events, 3u);                                 // balance, one position, done

        Command qb{};
        qb.type      = CommandType::QUERY_BOOK;
        qb.symbol    = 0;
        qb.qty       = 5;
        qb.clientRef = 3;
        j.process(qb, [&](const Event&) { ++events; });
        EXPECT_GT(events, 3u);
        j.flush();
    }

    std::uint64_t commands = 0;
    const JournalReadResult res = readJournal(file.path(), [&](const JournalRecord& r) {
        if (r.kind == RecordKind::COMMAND) ++commands;
    });
    EXPECT_EQ(res.status, JournalStatus::CLEAN);
    EXPECT_EQ(commands, 1u);                                   // only the order

    JournaledCore again(1, file.path());                       // and the journal still recovers
    EXPECT_EQ(again.recovery().read.status, JournalStatus::CLEAN);
    EXPECT_EQ(again.core().exchange().accounts().account(0).reservedCash, toPaise(500) * 10);
}

// ---------- every journal has an identity ----------

TEST(JournalIdentity, AFreshJournalStartsWithAnIdThenThePrices) {
    TempFile file("id_fresh");
    std::uint64_t id = 0;
    {
        JournaledCore j(2, file.path(), {toPaise(2500), toPaise(800)});
        id = j.journalId();
    }
    EXPECT_NE(id, 0u);
    EXPECT_LT(id, std::uint64_t{1} << 52);                      // fits a JavaScript number exactly
    const auto recs = allRecords(file.path());
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(recs[0].kind, RecordKind::JOURNAL_ID);
    EXPECT_EQ(recs[0].orderId, id);
    EXPECT_EQ(recs[1].kind, RecordKind::REFERENCE_PRICE);
    EXPECT_EQ(recs[2].kind, RecordKind::REFERENCE_PRICE);
}

TEST(JournalIdentity, RestartingKeepsTheIdAndDoesNotWriteAnother) {
    TempFile file("id_restart");
    std::uint64_t id = 0;
    {
        JournaledCore j(1, file.path());
        id = j.journalId();
    }
    JournaledCore again(1, file.path());
    EXPECT_EQ(again.journalId(), id);
    EXPECT_EQ(again.recovery().journalId, id);
    EXPECT_EQ(allRecords(file.path()).size(), 2u);              // the id and one price, nothing more
}

TEST(JournalIdentity, EveryFreshJournalGetsItsOwnId) {
    TempFile a("id_a");
    TempFile b("id_b");
    JournaledCore ja(1, a.path());
    JournaledCore jb(1, b.path());
    EXPECT_NE(ja.journalId(), jb.journalId());
}

TEST(JournalIdentity, AnOlderJournalWithoutOneIsGivenOneAndKeepsItsData) {
    TempFile file("id_legacy");
    {
        JournalWriter w(file.path());                            // the old format: no id, no prices
        w.append(JournalRecord::forCreateAccount(0, toPaise(1000)));
        w.append(JournalRecord::forGrantShares(0, 0, 50, toPaise(500)));
    }
    std::uint64_t id = 0;
    {
        JournaledCore j(1, file.path());
        id = j.journalId();
        EXPECT_NE(id, 0u);
        EXPECT_EQ(j.core().exchange().accounts().account(0).cash, toPaise(1000));
        EXPECT_EQ(j.core().exchange().accounts().account(0).positions[0].qty, 50);
    }
    JournaledCore again(1, file.path());
    EXPECT_EQ(again.journalId(), id);                           // the same id on the next start
    EXPECT_EQ(again.core().exchange().accounts().account(0).cash, toPaise(1000));
    EXPECT_EQ(allRecords(file.path()).size(), 3u);              // account, grant, and the id written at the end
}

TEST(JournalIdentity, TwoIdsOrAZeroIdAreRefused) {
    {
        TempFile file("id_two");
        {
            JournalWriter w(file.path());
            w.append(JournalRecord::forJournalId(5));
            w.append(JournalRecord::forJournalId(6));
        }
        EXPECT_THROW(JournaledCore(1, file.path()), std::runtime_error);
    }
    {
        TempFile file("id_zero");
        {
            JournalWriter w(file.path());
            w.append(JournalRecord::forJournalId(0));
        }
        EXPECT_THROW(JournaledCore(1, file.path()), std::runtime_error);
    }
}

TEST(JournalIdentity, TheIdIsAnsweredByQueryInfoAndNeverJournaled) {
    TempFile file("id_query");
    {
        JournaledCore j(1, file.path());
        Command q{};
        q.type      = CommandType::QUERY_INFO;
        q.clientRef = 9;
        std::vector<Event> ev;
        j.process(q, [&](const Event& e) { ev.push_back(e); });
        ASSERT_EQ(ev.size(), 2u);
        EXPECT_EQ(ev[0].type, EventType::ENGINE_INFO);
        EXPECT_EQ(static_cast<std::uint64_t>(ev[0].qty), j.journalId());
        EXPECT_EQ(ev[1].type, EventType::QUERY_DONE);
        j.flush();
    }
    std::uint64_t commands = 0;
    readJournal(file.path(), [&](const JournalRecord& r) {
        if (r.kind == RecordKind::COMMAND) ++commands;
    });
    EXPECT_EQ(commands, 0u);
}
