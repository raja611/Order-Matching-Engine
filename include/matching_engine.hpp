#pragma once

#include "order_book.hpp"
#include <unordered_map>
#include <memory>
#include <iostream>
#include <iomanip>

namespace ome {

class MatchingEngine {
public:
    struct Stats {
        uint64_t orders_submitted = 0;
        uint64_t orders_filled    = 0;
        uint64_t orders_partial   = 0;
        uint64_t orders_cancelled = 0;
        uint64_t orders_rejected  = 0;
        uint64_t trades_executed  = 0;
        uint64_t total_volume     = 0;
    };

    explicit MatchingEngine(size_t arena_capacity = 4'000'000)
        : arena_(arena_capacity) {}

    void add_symbol(Symbol sym, Price min_price, Price max_price) {
        books_.emplace(sym, std::make_unique<FlatOrderBook>(
            sym, min_price, max_price, arena_.base()));
    }

    OrderId submit_order(Symbol sym, Side side, OrderType type, Price price, Quantity quantity) noexcept
    {
        auto it = books_.find(sym);
        if (__builtin_expect(it == books_.end(), 0)) {
            ++stats_.orders_rejected;
            return 0;
        }

        Order* o = arena_.allocate(sym, side, type, price, quantity);
        ++stats_.orders_submitted;

        auto r = it->second->add_order(o);
        stats_.trades_executed += r.num_trades;
        stats_.total_volume   += r.total_filled;

        if (o->is_filled())              ++stats_.orders_filled;
        else if (o->filled_quantity > 0) ++stats_.orders_partial;

        return o->id;
    }

    bool cancel_order(Symbol sym, OrderId id) noexcept {
        auto it = books_.find(sym);
        if (it == books_.end()) return false;
        bool ok = it->second->cancel_order(arena_.get(id));
        if (ok) ++stats_.orders_cancelled;
        return ok;
    }

    const FlatOrderBook* get_book(Symbol sym) const {
        auto it = books_.find(sym);
        return it != books_.end() ? it->second.get() : nullptr;
    }

    const Stats& stats() const noexcept { return stats_; }

    void print_book(Symbol sym, size_t depth = 5) const {
        auto* book = get_book(sym);
        if (!book) { std::cout << "Symbol " << sym << " not found.\n"; return; }

        std::vector<FlatOrderBook::LevelInfo> bids, asks;
        book->top_of_book(depth, bids, asks);

        std::cout << "\n========== Order Book (Symbol " << sym << ") ==========\n";
        std::cout << std::setw(12) << "BID QTY" << " | "
                  << std::setw(10) << "BID PRICE"
                  << " || "
                  << std::setw(10) << "ASK PRICE" << " | "
                  << std::setw(12) << "ASK QTY" << "\n";
        std::cout << std::string(60, '-') << "\n";

        size_t max_levels = std::max(bids.size(), asks.size());
        for (size_t i = 0; i < max_levels; ++i) {
            if (i < bids.size()) {
                auto& b = bids[i];
                std::cout << std::setw(12) << b.quantity << " | "
                          << std::setw(7) << (b.price / 100) << "."
                          << std::setfill('0') << std::setw(2) << (b.price % 100)
                          << std::setfill(' ');
            } else {
                std::cout << std::setw(12) << "" << " | " << std::setw(10) << "";
            }
            std::cout << " || ";
            if (i < asks.size()) {
                auto& a = asks[i];
                std::cout << std::setw(7) << (a.price / 100) << "."
                          << std::setfill('0') << std::setw(2) << (a.price % 100)
                          << std::setfill(' ')
                          << " | " << std::setw(12) << a.quantity;
            }
            std::cout << "\n";
        }
        std::cout << "================================================\n";
        std::cout << "Resting: " << book->resting_orders()
                  << " | Trades: " << book->trade_count()
                  << " | Volume: " << book->total_volume() << "\n\n";
    }

    void print_stats() const {
        std::cout << "\n--- Engine Stats ---\n"
                  << "Submitted : " << stats_.orders_submitted << "\n"
                  << "Filled    : " << stats_.orders_filled << "\n"
                  << "Partial   : " << stats_.orders_partial << "\n"
                  << "Cancelled : " << stats_.orders_cancelled << "\n"
                  << "Rejected  : " << stats_.orders_rejected << "\n"
                  << "Trades    : " << stats_.trades_executed << "\n"
                  << "Volume    : " << stats_.total_volume << "\n"
                  << "--------------------\n\n";
    }

private:
    std::unordered_map<Symbol, std::unique_ptr<FlatOrderBook>> books_;
    OrderArena arena_;
    Stats      stats_;
};

} // namespace ome
