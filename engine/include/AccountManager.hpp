#pragma once
#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>
#include "Types.hpp"

namespace exch {

struct Position {
    Qty   qty         = 0;   // shares owned
    Qty   reservedQty = 0;   // shares locked by open SELL orders
    Price costBasis   = 0;   // total paise paid for the shares currently held

    Qty   available() const { return qty - reservedQty; }
    Price avgCost()   const { return qty > 0 ? costBasis / qty : 0; }
};

struct Account {
    Price                 cash         = 0;   // paise
    Price                 reservedCash = 0;   // paise locked by open BUY orders
    std::vector<Position> positions;          // indexed by SymbolId

    Price availableCash() const { return cash - reservedCash; }
};

class AccountManager {
public:
    explicit AccountManager(std::size_t numSymbols) : numSymbols_(numSymbols) {}

    // User ids are sequential (0, 1, 2...), so lookup is a plain array index.
    UserId createAccount(Price initialCash) {
        Account a;
        a.cash = initialCash;
        a.positions.resize(numSymbols_);
        accounts_.push_back(std::move(a));
        return static_cast<UserId>(accounts_.size() - 1);
    }

    // For seeding test data and initial holdings.
    void grantShares(UserId u, SymbolId s, Qty qty, Price costPerShare) {
        Position& p = accounts_[u].positions[s];
        p.qty       += qty;
        p.costBasis += qty * costPerShare;
    }

    bool           exists(UserId u)  const { return u < accounts_.size(); }
    const Account& account(UserId u) const { assert(exists(u)); return accounts_[u]; }

    // ---- reservations ----
    bool reserveCash(UserId u, Price amount) {
        Account& a = accounts_[u];
        if (amount > a.availableCash()) return false;
        a.reservedCash += amount;
        return true;
    }
    void releaseCash(UserId u, Price amount) {
        Account& a = accounts_[u];
        assert(a.reservedCash >= amount);
        a.reservedCash -= amount;
    }
    bool reserveShares(UserId u, SymbolId s, Qty qty) {
        Position& p = accounts_[u].positions[s];
        if (qty > p.available()) return false;
        p.reservedQty += qty;
        return true;
    }
    void releaseShares(UserId u, SymbolId s, Qty qty) {
        Position& p = accounts_[u].positions[s];
        assert(p.reservedQty >= qty);
        p.reservedQty -= qty;
    }

    // ---- settlement (one side of a trade each) ----
    // reservedPx = per-share price that was reserved for this buyer.
    void settleBuy(UserId buyer, SymbolId s, Price reservedPx, Price tradePx, Qty qty) {
        Account& a = accounts_[buyer];
        assert(a.reservedCash >= reservedPx * qty);
        a.reservedCash -= reservedPx * qty;   // release what was locked
        a.cash         -= tradePx * qty;      // pay what was actually owed
        Position& p = a.positions[s];
        p.qty       += qty;
        p.costBasis += tradePx * qty;
    }
    void settleSell(UserId seller, SymbolId s, Price tradePx, Qty qty) {
        Account& a = accounts_[seller];
        Position& p = a.positions[s];
        assert(p.qty >= qty && p.reservedQty >= qty);
        p.costBasis -= p.costBasis * qty / p.qty;   // remove proportional cost
        p.qty         -= qty;
        p.reservedQty -= qty;
        a.cash        += tradePx * qty;
    }

    // ---- totals, used by the conservation tests ----
    Price totalCash() const {
        Price sum = 0;
        for (const Account& a : accounts_) sum += a.cash;
        return sum;
    }
    Qty totalShares(SymbolId s) const {
        Qty sum = 0;
        for (const Account& a : accounts_) sum += a.positions[s].qty;
        return sum;
    }

private:
    std::size_t          numSymbols_;
    std::vector<Account> accounts_;
};

}  // namespace exch