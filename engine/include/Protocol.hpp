#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Messages.hpp"
#include "Types.hpp"

// Wire format between the gateway and the engine (TCP byte stream).
//
//   frame = u32 length | u8 type | body        (all numbers little-endian)
//   length counts the type byte plus the body; 1 <= length <= kMaxFrameBody.
//
//   1  NEW_ORDER    body 32: side u8, orderType u8, symbol u16, user u32,
//                            price i64, qty i64, clientRef u64
//   2  CANCEL_ORDER body 22: symbol u16, user u32, orderId u64, clientRef u64
//   3  QUERY_ACCOUNT body 12: user u32, clientRef u64
//   4  QUERY_BOOK    body 14: symbol u16, depth u32, clientRef u64
//   16 EVENT        body 108: type u8, reason u8, status u8, side u8, symbol u16, user u32,
//                            orderId u64, price i64, qty i64, remaining i64, clientRef u64,
//                            then the trade: tradeId u64, symbolId u16, price i64, qty i64,
//                            buyOrderId u64, sellOrderId u64, buyerId u32, sellerId u32, seq u64
//
// Order ids carry the symbol in their top 16 bits, so they do not fit a JavaScript number:
// use BigInt on the Node side.
namespace exch::wire {

enum class MsgType : std::uint8_t {
    NEW_ORDER = 1, CANCEL_ORDER = 2, QUERY_ACCOUNT = 3, QUERY_BOOK = 4, EVENT = 16
};

inline constexpr std::size_t kLenBytes         = 4;
inline constexpr std::size_t kMaxFrameBody     = 1024;   // largest legal value of the length field
inline constexpr std::size_t kNewOrderBody     = 32;
inline constexpr std::size_t kCancelBody       = 22;
inline constexpr std::size_t kQueryAccountBody = 12;
inline constexpr std::size_t kQueryBookBody    = 14;
inline constexpr std::size_t kEventBody        = 108;

class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void u8(std::uint8_t v)   { out_.push_back(v); }
    void u16(std::uint16_t v) { put(v, 2); }
    void u32(std::uint32_t v) { put(v, 4); }
    void u64(std::uint64_t v) { put(v, 8); }
    void i64(std::int64_t v)  { put(static_cast<std::uint64_t>(v), 8); }
private:
    void put(std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    std::vector<std::uint8_t>& out_;
};

// Callers check the body length first, so reads never run past the end.
class ByteReader {
public:
    ByteReader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    std::uint8_t  u8()  { return static_cast<std::uint8_t>(get(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(get(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
    std::uint64_t u64() { return get(8); }
    std::int64_t  i64() { return static_cast<std::int64_t>(get(8)); }
private:
    std::uint64_t get(std::size_t bytes) {
        assert(pos_ + bytes <= n_);
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < bytes; ++i)
            v |= static_cast<std::uint64_t>(p_[pos_ + i]) << (8 * i);
        pos_ += bytes;
        return v;
    }
    const std::uint8_t* p_;
    std::size_t         n_;
    std::size_t         pos_ = 0;
};

inline void beginFrame(std::vector<std::uint8_t>& out, MsgType type, std::size_t bodyLen) {
    ByteWriter w(out);
    w.u32(static_cast<std::uint32_t>(1 + bodyLen));
    w.u8(static_cast<std::uint8_t>(type));
}

// Appends one frame (NEW_ORDER or CANCEL_ORDER, chosen by c.type) to `out`.
// Appends one frame to `out`; the message type follows c.type.
inline void encodeCommand(const Command& c, std::vector<std::uint8_t>& out) {
    ByteWriter w(out);
    switch (c.type) {
        case CommandType::NEW_ORDER:
            beginFrame(out, MsgType::NEW_ORDER, kNewOrderBody);
            w.u8(static_cast<std::uint8_t>(c.side));
            w.u8(static_cast<std::uint8_t>(c.orderType));
            w.u16(c.symbol);
            w.u32(c.user);
            w.i64(c.price);
            w.i64(c.qty);
            w.u64(c.clientRef);
            break;
        case CommandType::CANCEL_ORDER:
            beginFrame(out, MsgType::CANCEL_ORDER, kCancelBody);
            w.u16(c.symbol);
            w.u32(c.user);
            w.u64(c.orderId);
            w.u64(c.clientRef);
            break;
        case CommandType::QUERY_ACCOUNT:
            beginFrame(out, MsgType::QUERY_ACCOUNT, kQueryAccountBody);
            w.u32(c.user);
            w.u64(c.clientRef);
            break;
        case CommandType::QUERY_BOOK: {
            beginFrame(out, MsgType::QUERY_BOOK, kQueryBookBody);
            const Qty depth = c.qty < 0 ? 0 : (c.qty > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : c.qty);
            w.u16(c.symbol);
            w.u32(static_cast<std::uint32_t>(depth));
            w.u64(c.clientRef);
            break;
        }
    }
}

// Appends one EVENT frame to `out`.
inline void encodeEvent(const Event& e, std::vector<std::uint8_t>& out) {
    beginFrame(out, MsgType::EVENT, kEventBody);
    ByteWriter w(out);
    w.u8(static_cast<std::uint8_t>(e.type));
    w.u8(static_cast<std::uint8_t>(e.reason));
    w.u8(static_cast<std::uint8_t>(e.status));
    w.u8(static_cast<std::uint8_t>(e.side));
    w.u16(e.symbol);
    w.u32(e.user);
    w.u64(e.orderId);
    w.i64(e.price);
    w.i64(e.qty);
    w.i64(e.remaining);
    w.u64(e.clientRef);
    w.u64(e.trade.tradeId);
    w.u16(e.trade.symbolId);
    w.i64(e.trade.price);
    w.i64(e.trade.qty);
    w.u64(e.trade.buyOrderId);
    w.u64(e.trade.sellOrderId);
    w.u32(e.trade.buyerId);
    w.u32(e.trade.sellerId);
    w.u64(e.trade.seq);
}

// ---- decoders: false means a wrong length or an invalid enum value; `out` is untouched ----

inline bool decodeNewOrder(const std::uint8_t* body, std::size_t len, Command& out) {
    if (len != kNewOrderBody) return false;
    ByteReader r(body, len);
    const std::uint8_t side = r.u8();
    const std::uint8_t ot   = r.u8();
    if (side > 1 || ot > 1) return false;
    Command c{};
    c.type      = CommandType::NEW_ORDER;
    c.side      = static_cast<Side>(side);
    c.orderType = static_cast<OrderType>(ot);
    c.symbol    = r.u16();
    c.user      = r.u32();
    c.price     = r.i64();
    c.qty       = r.i64();
    c.clientRef = r.u64();
    out = c;
    return true;
}

inline bool decodeCancel(const std::uint8_t* body, std::size_t len, Command& out) {
    if (len != kCancelBody) return false;
    ByteReader r(body, len);
    Command c{};
    c.type      = CommandType::CANCEL_ORDER;
    c.symbol    = r.u16();
    c.user      = r.u32();
    c.orderId   = r.u64();
    c.clientRef = r.u64();
    out = c;
    return true;
}



// Decodes any gateway -> engine frame. false = unknown message type or a malformed body.
inline bool decodeQueryAccount(const std::uint8_t* body, std::size_t len, Command& out) {
    if (len != kQueryAccountBody) return false;
    ByteReader r(body, len);
    Command c{};
    c.type      = CommandType::QUERY_ACCOUNT;
    c.user      = r.u32();
    c.clientRef = r.u64();
    out = c;
    return true;
}

inline bool decodeQueryBook(const std::uint8_t* body, std::size_t len, Command& out) {
    if (len != kQueryBookBody) return false;
    ByteReader r(body, len);
    Command c{};
    c.type      = CommandType::QUERY_BOOK;
    c.symbol    = r.u16();
    c.qty       = r.u32();                // the depth
    c.clientRef = r.u64();
    out = c;
    return true;
}

// Decodes any gateway -> engine frame. false = unknown message type or a malformed body.
inline bool decodeCommandFrame(std::uint8_t type, const std::uint8_t* body, std::size_t len,
                               Command& out) {
    if (type == static_cast<std::uint8_t>(MsgType::NEW_ORDER))     return decodeNewOrder(body, len, out);
    if (type == static_cast<std::uint8_t>(MsgType::CANCEL_ORDER))  return decodeCancel(body, len, out);
    if (type == static_cast<std::uint8_t>(MsgType::QUERY_ACCOUNT)) return decodeQueryAccount(body, len, out);
    if (type == static_cast<std::uint8_t>(MsgType::QUERY_BOOK))    return decodeQueryBook(body, len, out);
    return false;
}

inline bool decodeEvent(const std::uint8_t* body, std::size_t len, Event& out) {
    if (len != kEventBody) return false;
    ByteReader r(body, len);
    const std::uint8_t type   = r.u8();
    const std::uint8_t reason = r.u8();
    const std::uint8_t status = r.u8();
    const std::uint8_t side   = r.u8();
    if (type > 9 || reason > 8 || status > 4 || side > 1) return false;
    Event e;
    e.type      = static_cast<EventType>(type);
    e.reason    = static_cast<RejectReason>(reason);
    e.status    = static_cast<OrderStatus>(status);
    e.side      = static_cast<Side>(side);
    e.symbol    = r.u16();
    e.user      = r.u32();
    e.orderId   = r.u64();
    e.price     = r.i64();
    e.qty       = r.i64();
    e.remaining = r.i64();
    e.clientRef = r.u64();
    e.trade.tradeId     = r.u64();
    e.trade.symbolId    = r.u16();
    e.trade.price       = r.i64();
    e.trade.qty         = r.i64();
    e.trade.buyOrderId  = r.u64();
    e.trade.sellOrderId = r.u64();
    e.trade.buyerId     = r.u32();
    e.trade.sellerId    = r.u32();
    e.trade.seq         = r.u64();
    out = e;
    return true;
}

// Turns a TCP byte stream into frames. Feed it whatever recv() returned, then call
// next() until it stops returning FRAME. Not thread-safe: one reader per connection.
class FrameReader {
public:
    enum class Result { NEED_MORE, FRAME, BAD_FRAME };

    void feed(const std::uint8_t* data, std::size_t n) {
        if (bad_) return;
        if (pos_ > 0 && (pos_ == buf_.size() || pos_ >= kCompactAt)) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
            pos_ = 0;
        }
        buf_.insert(buf_.end(), data, data + n);
    }

    // On FRAME, `body` points at the message body. It stays valid until the next feed().
    // BAD_FRAME means the peer broke the protocol: close the connection.
    Result next(std::uint8_t& type, const std::uint8_t*& body, std::size_t& bodyLen) {
        if (bad_) return Result::BAD_FRAME;
        const std::size_t avail = buf_.size() - pos_;
        if (avail < kLenBytes) return Result::NEED_MORE;

        std::uint32_t len = 0;
        for (std::size_t i = 0; i < kLenBytes; ++i)
            len |= static_cast<std::uint32_t>(buf_[pos_ + i]) << (8 * i);
        if (len < 1 || len > kMaxFrameBody) {
            bad_ = true;
            return Result::BAD_FRAME;
        }
        if (avail < kLenBytes + len) return Result::NEED_MORE;

        type    = buf_[pos_ + kLenBytes];
        body    = buf_.data() + pos_ + kLenBytes + 1;
        bodyLen = len - 1;
        pos_   += kLenBytes + len;
        return Result::FRAME;
    }

    bool        bad()      const { return bad_; }
    std::size_t buffered() const { return buf_.size() - pos_; }

private:
    static constexpr std::size_t kCompactAt = 64 * 1024;
    std::vector<std::uint8_t> buf_;
    std::size_t               pos_ = 0;
    bool                      bad_ = false;
};

}  // namespace exch::wire