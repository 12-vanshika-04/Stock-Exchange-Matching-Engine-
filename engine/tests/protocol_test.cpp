#include <gtest/gtest.h>
#include <random>
#include <vector>
#include "Protocol.hpp"

using namespace exch;
using namespace exch::wire;
using Bytes = std::vector<std::uint8_t>;

namespace {

Command newOrderCmd() {
    Command c{};
    c.type      = CommandType::NEW_ORDER;
    c.side      = Side::SELL;
    c.orderType = OrderType::LIMIT;
    c.symbol    = 5;
    c.user      = 0x01020304;
    c.price     = 50000;
    c.qty       = 100;
    c.clientRef = 0x0102030405060708ULL;
    return c;
}

Command cancelCmd() {
    Command c{};
    c.type      = CommandType::CANCEL_ORDER;
    c.symbol    = 3;
    c.user      = 7;
    c.orderId   = makeOrderId(3, 9);
    c.clientRef = 0x11;
    return c;
}

Event fullEvent() {
    Event e;
    e.type      = EventType::TRADE;
    e.reason    = RejectReason::PRICE_OUT_OF_BAND;
    e.status    = OrderStatus::PARTIAL;
    e.side      = Side::SELL;
    e.symbol    = 4;
    e.user      = 123456;
    e.orderId   = makeOrderId(4, 77);
    e.price     = -12345;
    e.qty       = 9'876'543'210LL;
    e.remaining = 42;
    e.clientRef = 0xDEADBEEFCAFEULL;
    e.trade     = Trade{99, 4, 50500, 17, makeOrderId(4, 5), makeOrderId(4, 6), 11, 22, 1234};
    return e;
}

bool sameCommand(const Command& a, const Command& b) {
    return a.type == b.type && a.side == b.side && a.orderType == b.orderType &&
           a.symbol == b.symbol && a.user == b.user && a.price == b.price &&
           a.qty == b.qty && a.orderId == b.orderId && a.clientRef == b.clientRef;
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

// Reads exactly one frame from `frame` and returns its type and body.
void readOne(const Bytes& frame, std::uint8_t& type, Bytes& body) {
    FrameReader r;
    r.feed(frame.data(), frame.size());
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    ASSERT_EQ(r.next(type, p, n), FrameReader::Result::FRAME);
    body.assign(p, p + n);
    EXPECT_EQ(r.buffered(), 0u);
}

}  // namespace

// ---- the exact bytes: this is the contract the Node gateway must match ----

TEST(Wire, NewOrderHasTheDocumentedBytes) {
    Bytes out;
    encodeCommand(newOrderCmd(), out);
    const Bytes expected = {
        0x21, 0x00, 0x00, 0x00,                          // length = 33 (type + 32 body bytes)
        0x01,                                            // NEW_ORDER
        0x01, 0x00,                                      // side SELL, order type LIMIT
        0x05, 0x00,                                      // symbol 5
        0x04, 0x03, 0x02, 0x01,                          // user 0x01020304
        0x50, 0xC3, 0, 0, 0, 0, 0, 0,                    // price 50000
        0x64, 0, 0, 0, 0, 0, 0, 0,                       // qty 100
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}; // clientRef
    EXPECT_EQ(out, expected);
}

TEST(Wire, CancelHasTheDocumentedBytes) {
    Bytes out;
    encodeCommand(cancelCmd(), out);
    const Bytes expected = {
        0x17, 0x00, 0x00, 0x00,                          // length = 23
        0x02,                                            // CANCEL_ORDER
        0x03, 0x00,                                      // symbol 3
        0x07, 0x00, 0x00, 0x00,                          // user 7
        0x09, 0, 0, 0, 0, 0, 0x03, 0x00,                 // order id: local 9, symbol 3 in the top 16 bits
        0x11, 0, 0, 0, 0, 0, 0, 0};                      // clientRef 0x11
    EXPECT_EQ(out, expected);
}

TEST(Wire, EventHasAFixedSize) {
    Bytes plain, full;
    encodeEvent(Event{}, plain);
    encodeEvent(fullEvent(), full);
    EXPECT_EQ(plain.size(), kLenBytes + 1 + kEventBody);       // 113 bytes
    EXPECT_EQ(full.size(), plain.size());
}

// ---- round trips ----

TEST(Wire, CommandsRoundTrip) {
    for (const Command& in : {newOrderCmd(), cancelCmd()}) {
        Bytes frame;
        encodeCommand(in, frame);
        std::uint8_t type = 0;
        Bytes body;
        readOne(frame, type, body);
        Command out{};
        ASSERT_TRUE(decodeCommandFrame(type, body.data(), body.size(), out));
        EXPECT_TRUE(sameCommand(in, out));
    }
}

TEST(Wire, EventsRoundTrip) {
    for (const Event& in : {fullEvent(), Event{}}) {
        Bytes frame;
        encodeEvent(in, frame);
        std::uint8_t type = 0;
        Bytes body;
        readOne(frame, type, body);
        EXPECT_EQ(type, static_cast<std::uint8_t>(MsgType::EVENT));
        Event out;
        ASSERT_TRUE(decodeEvent(body.data(), body.size(), out));
        EXPECT_TRUE(sameEvent(in, out));
    }
}

TEST(Wire, UnknownMessageTypeIsRejected) {
    Bytes frame;
    encodeCommand(newOrderCmd(), frame);
    std::uint8_t type = 0;
    Bytes body;
    readOne(frame, type, body);
    Command out{};
    EXPECT_FALSE(decodeCommandFrame(99, body.data(), body.size(), out));
    EXPECT_FALSE(decodeCommandFrame(static_cast<std::uint8_t>(MsgType::EVENT),
                                    body.data(), body.size(), out));    // events do not flow this way
}

// ---- TCP gives a byte stream, not messages ----

TEST(Wire, FramesSplitAcrossReadsAreReassembled) {
    Bytes stream;
    encodeCommand(newOrderCmd(), stream);
    encodeCommand(cancelCmd(), stream);
    encodeEvent(fullEvent(), stream);

    FrameReader r;
    std::vector<std::uint8_t> types;
    for (std::uint8_t byte : stream) {                  // one byte per "read"
        r.feed(&byte, 1);
        std::uint8_t type = 0;
        const std::uint8_t* p = nullptr;
        std::size_t n = 0;
        while (r.next(type, p, n) == FrameReader::Result::FRAME) types.push_back(type);
    }
    const std::vector<std::uint8_t> expected = {1, 2, 16};
    EXPECT_EQ(types, expected);
    EXPECT_EQ(r.buffered(), 0u);
}

TEST(Wire, SeveralFramesInOneReadAreAllDelivered) {
    Bytes stream;
    for (int i = 0; i < 50; ++i) {
        encodeCommand(newOrderCmd(), stream);
        encodeCommand(cancelCmd(), stream);
    }
    FrameReader r;
    r.feed(stream.data(), stream.size());

    std::size_t news = 0, cancels = 0;
    std::uint8_t type = 0;
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    while (r.next(type, p, n) == FrameReader::Result::FRAME) {
        Command c{};
        ASSERT_TRUE(decodeCommandFrame(type, p, n, c));
        if (c.type == CommandType::NEW_ORDER) ++news;
        else                                  ++cancels;
    }
    EXPECT_EQ(news, 50u);
    EXPECT_EQ(cancels, 50u);
}

TEST(Wire, ReaderSurvivesBufferCompaction) {
    Bytes frame;
    encodeCommand(newOrderCmd(), frame);
    FrameReader r;
    std::size_t got = 0;
    std::uint8_t type = 0;
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    auto drainOne = [&]() {
        if (r.next(type, p, n) != FrameReader::Result::FRAME) return false;
        Command c{};
        EXPECT_TRUE(decodeCommandFrame(type, p, n, c));
        EXPECT_TRUE(sameCommand(c, newOrderCmd()));
        ++got;
        return true;
    };
    for (int i = 0; i < 5000; ++i) {
        r.feed(frame.data(), frame.size());
        r.feed(frame.data(), frame.size());
        drainOne();                                     // consume one, leave a growing backlog
    }
    while (drainOne()) {}
    EXPECT_EQ(got, 10000u);
    EXPECT_EQ(r.buffered(), 0u);
}

// ---- hostile or broken peers ----

TEST(Wire, ZeroOrOversizedLengthIsAProtocolError) {
    std::uint8_t type = 0;
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;

    FrameReader zero;
    const std::uint8_t zeroLen[4] = {0, 0, 0, 0};
    zero.feed(zeroLen, 4);
    EXPECT_EQ(zero.next(type, p, n), FrameReader::Result::BAD_FRAME);
    EXPECT_TRUE(zero.bad());

    FrameReader big;
    const std::uint8_t bigLen[4] = {0x01, 0x04, 0, 0};  // 1025 > kMaxFrameBody
    big.feed(bigLen, 4);
    EXPECT_EQ(big.next(type, p, n), FrameReader::Result::BAD_FRAME);

    Bytes good;                                         // once bad, a reader stays bad
    encodeCommand(newOrderCmd(), good);
    big.feed(good.data(), good.size());
    EXPECT_EQ(big.next(type, p, n), FrameReader::Result::BAD_FRAME);
}

TEST(Wire, DecodersRejectWrongLengthsAndInvalidValues) {
    Bytes frame;
    encodeCommand(newOrderCmd(), frame);
    const Bytes body(frame.begin() + 5, frame.end());   // skip the length (4) and the type (1)
    Command c{};
    EXPECT_TRUE(decodeNewOrder(body.data(), body.size(), c));
    EXPECT_FALSE(decodeNewOrder(body.data(), body.size() - 1, c));
    Bytes longer = body;
    longer.push_back(0);
    EXPECT_FALSE(decodeNewOrder(longer.data(), longer.size(), c));
    Bytes badSide = body;
    badSide[0] = 2;                                     // side must be 0 or 1
    EXPECT_FALSE(decodeNewOrder(badSide.data(), badSide.size(), c));
    Bytes badOrderType = body;
    badOrderType[1] = 9;
    EXPECT_FALSE(decodeNewOrder(badOrderType.data(), badOrderType.size(), c));

    Bytes eFrame;
    encodeEvent(fullEvent(), eFrame);
    const Bytes eBody(eFrame.begin() + 5, eFrame.end());
    Event e;
    EXPECT_TRUE(decodeEvent(eBody.data(), eBody.size(), e));
    EXPECT_FALSE(decodeEvent(eBody.data(), eBody.size() - 1, e));
    for (std::size_t field = 0; field < 4; ++field) {   // type, reason, status, side
        Bytes bad = eBody;
        bad[field] = 99;
        EXPECT_FALSE(decodeEvent(bad.data(), bad.size(), e)) << "field " << field;
    }
}

TEST(Wire, GarbageNeverCrashesTheReaderOrDecoders) {
    std::mt19937 rng(5);
    std::size_t frames = 0;
    for (int round = 0; round < 3000; ++round) {
        Bytes junk(rng() % 300);
        for (std::uint8_t& b : junk) b = static_cast<std::uint8_t>(rng());
        if (junk.size() >= 5 && (rng() & 1)) {          // a plausible length, so decoders get reached
            junk[0] = static_cast<std::uint8_t>(1 + rng() % 120);
            junk[1] = junk[2] = junk[3] = 0;
        }
        FrameReader r;
        r.feed(junk.data(), junk.size());
        std::uint8_t type = 0;
        const std::uint8_t* p = nullptr;
        std::size_t n = 0;
        while (r.next(type, p, n) == FrameReader::Result::FRAME) {
            ++frames;
            Command c{};
            Event e;
            decodeNewOrder(p, n, c);
            decodeCancel(p, n, c);
            decodeEvent(p, n, e);
        }
    }
    EXPECT_GT(frames, 0u);
}
// ---- queries ----

TEST(Wire, QueryAccountHasTheDocumentedBytes) {
    Command c{};
    c.type      = CommandType::QUERY_ACCOUNT;
    c.user      = 7;
    c.clientRef = 0x11;
    Bytes out;
    encodeCommand(c, out);
    const Bytes expected = {
        0x0D, 0x00, 0x00, 0x00,                          // length = 13
        0x03,                                            // QUERY_ACCOUNT
        0x07, 0x00, 0x00, 0x00,                          // user 7
        0x11, 0, 0, 0, 0, 0, 0, 0};                      // clientRef
    EXPECT_EQ(out, expected);
}

TEST(Wire, QueryBookHasTheDocumentedBytes) {
    Command c{};
    c.type      = CommandType::QUERY_BOOK;
    c.symbol    = 3;
    c.qty       = 20;
    c.clientRef = 0x22;
    Bytes out;
    encodeCommand(c, out);
    const Bytes expected = {
        0x0F, 0x00, 0x00, 0x00,                          // length = 15
        0x04,                                            // QUERY_BOOK
        0x03, 0x00,                                      // symbol 3
        0x14, 0x00, 0x00, 0x00,                          // depth 20
        0x22, 0, 0, 0, 0, 0, 0, 0};                      // clientRef
    EXPECT_EQ(out, expected);
}

TEST(Wire, QueriesRoundTripAndRefuseWrongLengths) {
    Command a{};
    a.type      = CommandType::QUERY_ACCOUNT;
    a.user      = 123456;
    a.clientRef = 0xABCDEF;
    Command b{};
    b.type      = CommandType::QUERY_BOOK;
    b.symbol    = 5;
    b.qty       = 200;
    b.clientRef = 0x1234;
    for (const Command& in : {a, b}) {
        Bytes frame;
        encodeCommand(in, frame);
        std::uint8_t type = 0;
        Bytes body;
        readOne(frame, type, body);
        Command out{};
        ASSERT_TRUE(decodeCommandFrame(type, body.data(), body.size(), out));
        EXPECT_TRUE(sameCommand(in, out));
        EXPECT_FALSE(decodeCommandFrame(type, body.data(), body.size() - 1, out));
    }
}

TEST(Wire, TheQueryEventTypesDecodeAndTheNextValueDoesNot) {
    for (EventType t : {EventType::ACCOUNT_BALANCE, EventType::ACCOUNT_POSITION, EventType::BOOK_LEVEL,
                        EventType::QUERY_DONE, EventType::QUERY_REJECTED}) {
        Event in;
        in.type      = t;
        in.symbol    = 2;
        in.user      = 9;
        in.price     = -5;
        in.qty       = 77;
        in.remaining = 3;
        in.clientRef = 42;
        Bytes frame;
        encodeEvent(in, frame);
        std::uint8_t type = 0;
        Bytes body;
        readOne(frame, type, body);
        Event out;
        ASSERT_TRUE(decodeEvent(body.data(), body.size(), out)) << static_cast<int>(t);
        EXPECT_TRUE(sameEvent(in, out));
    }
    Bytes frame;
    encodeEvent(Event{}, frame);
    Bytes body(frame.begin() + 5, frame.end());
    body[0] = 10;                                        // one past QUERY_REJECTED
    Event out;
    EXPECT_FALSE(decodeEvent(body.data(), body.size(), out));
}