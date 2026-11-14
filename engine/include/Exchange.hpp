#pragma once
#include <stdexcept>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include "AccountManager.hpp"
#include "MatchingEngine.hpp"
#include "Types.hpp"

namespace exch {

// Limits that guarantee price * qty can never overflow int64.
inline constexpr Price kMaxPrice = toPaise(1'000'000);   // Rs 10 lakh per share
inline constexpr Qty   kMaxQty   = 100'000'000;
inline constexpr Price kBandPercent = 20;   // limit prices must be within +-20% of the reference price

enum class RejectReason : std::uint8_t {
    NONE, UNKNOWN_USER, UNKNOWN_SYMBOL, BAD_QUANTITY, BAD_PRICE,
    INSUFFICIENT_FUNDS, INSUFFICIENT_SHARES, PRICE_OFF_TICK, PRICE_OUT_OF_BAND
};

struct ExchangeResult {
    Order              order;     // final state of the order
    std::vector<Trade> trades;
    RejectReason       reason;
};

class Exchange {
public:
               // referencePrices: one per symbol, in paise. Empty means Rs 500 for every symbol.
    // Limit prices must lie within +-20% of the symbol's reference price.
    explicit Exchange(std::size_t numSymbols, const std::vector<Price>& referencePrices = {})
        : numSymbols_(numSymbols),
          bands_(makeBands(numSymbols, referencePrices)),
          engine_(bands_),
          accounts_(numSymbols),
          nextLocalId_(numSymbols, 0) {}

    PriceBand band(SymbolId s) const { return bands_[s]; }
        std::size_t numSymbols() const { return numSymbols_; }
    // The band is symmetric around the reference price, so its midpoint recovers it exactly.
    Price referencePrice(SymbolId s) const { return (bands_[s].low + bands_[s].high) / 2; }

    AccountManager&       accounts()       { return accounts_; }
    const AccountManager& accounts() const { return accounts_; }
    const MatchingEngine& engine()   const { return engine_; }

    ExchangeResult submit(UserId user, SymbolId symbol, Side side,
                          OrderType type, Price price, Qty qty) {
                const OrderId id = (symbol < numSymbols_) ? makeOrderId(symbol, ++nextLocalId_[symbol]) : 0;
        Order o{id, user, symbol, side, type, OrderStatus::NEW,
                type == OrderType::LIMIT ? price : 0, qty, qty, 0};

        // 1. Validate
        
        if (!accounts_.exists(user))   return reject(o, RejectReason::UNKNOWN_USER);
        if (symbol >= numSymbols_)     return reject(o, RejectReason::UNKNOWN_SYMBOL);
        if (qty <= 0 || qty > kMaxQty) return reject(o, RejectReason::BAD_QUANTITY);
        if (type == OrderType::LIMIT && (price <= 0 || price > kMaxPrice))
            return reject(o, RejectReason::BAD_PRICE);
                    if (type == OrderType::LIMIT) {
            if (price % kTickSize != 0)
                return reject(o, RejectReason::PRICE_OFF_TICK);
            if (price < bands_[symbol].low || price > bands_[symbol].high)
                return reject(o, RejectReason::PRICE_OUT_OF_BAND);
        }

        // 2. Reserve what this order could cost
        if (side == Side::BUY) {
            const Price need = (type == OrderType::LIMIT) ? price * qty
                                                          : marketBuyCost(symbol, qty);
            if (!accounts_.reserveCash(user, need))
                return reject(o, RejectReason::INSUFFICIENT_FUNDS);
        } else {
            if (!accounts_.reserveShares(user, symbol, qty))
                return reject(o, RejectReason::INSUFFICIENT_SHARES);
        }

        // 3. Match
        SubmitResult m = engine_.submit(o);

        // 4. Settle every trade
        for (const Trade& t : m.trades) {
            // A limit BUY reserved at its own limit price; every other buyer
            // (resting, or a market BUY) reserved exactly the trade price.
            const Price reservedPx =
                (side == Side::BUY && type == OrderType::LIMIT) ? price : t.price;
            accounts_.settleBuy(t.buyerId, symbol, reservedPx, t.price, t.qty);
            accounts_.settleSell(t.sellerId, symbol, t.price, t.qty);
        }

        // 5. A market SELL's unfilled shares were reserved but will never rest
        if (type == OrderType::MARKET && side == Side::SELL && m.order.remaining > 0)
            accounts_.releaseShares(user, symbol, m.order.remaining);

        return ExchangeResult{m.order, std::move(m.trades), RejectReason::NONE};
    }

    // Only the owner can cancel. Releases whatever the order still holds.
    bool cancel(UserId user, SymbolId symbol, OrderId id) {
        if (symbol >= numSymbols_) return false;
        const Order* o = engine_.book(symbol).find(id);
        if (o == nullptr || o->userId != user) return false;

        const Side side = o->side;               // copy before the order is destroyed
        const Price px  = o->price;
        const Qty  rem  = o->remaining;
        engine_.cancel(symbol, id);

        if (side == Side::BUY) accounts_.releaseCash(user, px * rem);
        else                   accounts_.releaseShares(user, symbol, rem);
        return true;
    }

private:

        // Exact cost of buying `qty` right now, by walking the ask side read-only.
    // Stops as soon as `qty` shares are covered.
    Price marketBuyCost(SymbolId symbol, Qty qty) const {
        Price cost = 0;
        Qty   left = qty;
        engine_.book(symbol).forEachLevel(Side::SELL, [&](Price px, Qty levelQty) {
            const Qty take = std::min(left, levelQty);
            cost += px * take;
            left -= take;
            return left > 0;              // keep walking only while shares remain
        });
        return cost;
    }
        static std::vector<PriceBand> makeBands(std::size_t numSymbols, const std::vector<Price>& refs) {
        if (!refs.empty() && refs.size() != numSymbols)
            throw std::invalid_argument("need exactly one reference price per symbol");
        std::vector<PriceBand> bands;
        bands.reserve(numSymbols);
        for (std::size_t i = 0; i < numSymbols; ++i) {
            const Price ref = refs.empty() ? toPaise(500) : refs[i];
            if (ref <= 0 || ref % kTickSize != 0)
                throw std::invalid_argument("a reference price must be a positive multiple of the tick size");
            const Price low  = ref - ref * kBandPercent / 100;
            const Price high = ref + ref * kBandPercent / 100;
            if (low <= 0 || high > kMaxPrice || low % kTickSize != 0 || high % kTickSize != 0)
                throw std::invalid_argument("the +-20% band around a reference price must fall on the tick size");
            bands.push_back(PriceBand{low, high});
        }
        return bands;
    }

    static ExchangeResult reject(Order o, RejectReason why) {
        o.status = OrderStatus::REJECTED;
        return ExchangeResult{o, {}, why};
    }

        std::size_t                numSymbols_;
    std::vector<PriceBand>     bands_;       // per symbol: lowest and highest allowed limit price
    MatchingEngine             engine_;
    AccountManager             accounts_;
    std::vector<std::uint64_t> nextLocalId_;
};

}  // namespace exch