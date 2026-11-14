#pragma once
#include <cstdint>
#include <type_traits>
#include "Exchange.hpp"
#include "Types.hpp"

namespace exch {

enum class CommandType : std::uint8_t { NEW_ORDER, CANCEL_ORDER, QUERY_ACCOUNT, QUERY_BOOK };

// Queries only read: they change nothing and are never journaled.
constexpr bool isQuery(CommandType t) {
    return t == CommandType::QUERY_ACCOUNT || t == CommandType::QUERY_BOOK;
}

// The most price levels per side that one QUERY_BOOK returns.
inline constexpr Qty kMaxBookDepth = 200;

// What the gateway sends to the engine.
struct Command {
    CommandType   type;
    Side          side;
    OrderType     orderType;
    SymbolId      symbol;      // NEW_ORDER, CANCEL_ORDER, QUERY_BOOK
    UserId        user;        // every type except QUERY_BOOK
    Price         price;       // limit price (NEW_ORDER, LIMIT only)
    Qty           qty;         // NEW_ORDER: quantity. QUERY_BOOK: levels per side (clamped to 1..kMaxBookDepth)
    OrderId       orderId;     // CANCEL_ORDER only
    std::uint64_t clientRef;   // echoed back in every event this command causes
};

enum class EventType : std::uint8_t {
    ORDER_ACCEPTED,   // carries the order's state AFTER matching; TRADE events follow
    ORDER_REJECTED,
    TRADE,
    ORDER_CANCELLED,
    CANCEL_REJECTED,
    // Query answers: one command gives a run of events with the same clientRef,
    // ending in QUERY_DONE (or a single QUERY_REJECTED).
    ACCOUNT_BALANCE,   // user, price = cash (paise), qty = cash reserved by open buys
    ACCOUNT_POSITION,  // user, symbol, qty = shares owned, remaining = shares reserved by open sells,
                       // price = total cost basis (paise). One per symbol, in symbol order.
    BOOK_LEVEL,        // symbol, side (BUY = bid, SELL = ask), price, qty = total shares at that price.
                       // Bids best-first, then asks best-first.
    QUERY_DONE,
    QUERY_REJECTED     // reason = UNKNOWN_USER or UNKNOWN_SYMBOL
};

// What the engine sends back. Which fields matter depends on `type`.
struct Event {
    EventType     type      = EventType::ORDER_ACCEPTED;
    RejectReason  reason    = RejectReason::NONE;    // ORDER_REJECTED only
    OrderStatus   status    = OrderStatus::NEW;      // ORDER_ACCEPTED / REJECTED / CANCELLED
    Side          side      = Side::BUY;
    SymbolId      symbol    = 0;
    UserId        user      = 0;
    OrderId       orderId   = 0;
    Price         price     = 0;
    Qty           qty       = 0;                     // original order qty, or trade qty
    Qty           remaining = 0;
    std::uint64_t clientRef = 0;
    Trade         trade{};                           // TRADE only
};

// The SPSC queue copies items as raw bytes, so these must stay plain data.
static_assert(std::is_trivially_copyable_v<Command>);
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace exch