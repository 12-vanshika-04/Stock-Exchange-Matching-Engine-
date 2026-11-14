#pragma once
#include <cstdint>

namespace exch {

using OrderId  = std::uint64_t;
using UserId   = std::uint32_t;
using SymbolId = std::uint16_t;
using SeqNo    = std::uint64_t;
using Price    = std::int64_t;   // in paise: Rs 500.00 = 50000
using Qty      = std::int64_t;   // number of shares

enum class Side        : std::uint8_t { BUY, SELL };
enum class OrderType   : std::uint8_t { LIMIT, MARKET };
enum class OrderStatus : std::uint8_t { NEW, PARTIAL, FILLED, CANCELLED, REJECTED };

struct Order {
    OrderId     id;
    UserId      userId;
    SymbolId    symbolId;
    Side        side;
    OrderType   type;
    OrderStatus status;
    Price       price;       // ignored for MARKET orders
    Qty         qty;         // original quantity
    Qty         remaining;   // still unfilled
    SeqNo       seq;         // arrival order = time priority
};

struct Trade {
    std::uint64_t tradeId;
    SymbolId      symbolId;
    Price         price;     // the resting order's price
    Qty           qty;
    OrderId       buyOrderId;
    OrderId       sellOrderId;
    UserId        buyerId;
    UserId        sellerId;
    SeqNo         seq;
};
inline constexpr Price kTickSize = 5;   // Rs 0.05: limit prices must be multiples of this
// Rs 500.00 -> toPaise(500) = 50000, Rs 499.50 -> toPaise(499, 50) = 49950
constexpr Price toPaise(std::int64_t rupees, std::int64_t paise = 0) {
    return rupees * 100 + paise;
}
// An order id packs the symbol into its top 16 bits, so each symbol numbers its own
// orders 1, 2, 3... (compact per-book arrays) while ids stay unique across symbols.
constexpr int kOrderIdSymbolShift = 48;

constexpr OrderId makeOrderId(SymbolId symbol, std::uint64_t localSeq) {
    return (static_cast<OrderId>(symbol) << kOrderIdSymbolShift) | localSeq;
}
constexpr SymbolId symbolOfOrderId(OrderId id) {
    return static_cast<SymbolId>(id >> kOrderIdSymbolShift);
}
constexpr std::uint64_t localOrderId(OrderId id) {
    return id & ((OrderId{1} << kOrderIdSymbolShift) - 1);
}

}  // namespace exch