#pragma once

#include "types.hpp"
#include <vector>
#include <algorithm>

namespace ome {

struct PriceLevel {
    uint32_t head_idx = NULL_IDX;
    uint32_t tail_idx = NULL_IDX;
    Quantity total_qty = 0;
    uint32_t count     = 0;

    [[gnu::always_inline]]
    void push_back(uint32_t idx, Order* base) noexcept {
        Order& o = base[idx];
        o.prev_idx = tail_idx;
        o.next_idx = NULL_IDX;
        if (tail_idx != NULL_IDX) base[tail_idx].next_idx = idx;
        else                       head_idx = idx;
        tail_idx = idx;
        total_qty += o.remaining();
        ++count;
    }

    [[gnu::always_inline]]
    void unlink(uint32_t idx, Order* base) noexcept {
        Order& o = base[idx];
        if (o.prev_idx != NULL_IDX) base[o.prev_idx].next_idx = o.next_idx;
        else                         head_idx = o.next_idx;
        if (o.next_idx != NULL_IDX) base[o.next_idx].prev_idx = o.prev_idx;
        else                         tail_idx = o.prev_idx;
        total_qty -= o.remaining();
        --count;
    }

    [[gnu::always_inline]]
    uint32_t pop_front(Order* base) noexcept {
        uint32_t idx = head_idx;
        Order& o = base[idx];
        head_idx = o.next_idx;
        if (head_idx != NULL_IDX) base[head_idx].prev_idx = NULL_IDX;
        else                       tail_idx = NULL_IDX;
        total_qty -= o.remaining();
        --count;
        return idx;
    }

    bool empty() const noexcept { return head_idx == NULL_IDX; }
};

static_assert(sizeof(PriceLevel) == 16, "PriceLevel must be 16 bytes");

class FlatOrderBook {
public:
    struct MatchResult {
        uint32_t num_trades   = 0;
        Quantity total_filled = 0;
    };

    struct LevelInfo {
        Price    price;
        Quantity quantity;
        uint32_t order_count;
    };

    FlatOrderBook(Symbol symbol, Price min_price, Price max_price, Order* base)
        : symbol_(symbol), min_price_(min_price), max_price_(max_price),
          best_bid_(min_price - 1), best_ask_(max_price + 1),
          base_(base),
          levels_(static_cast<size_t>(max_price - min_price + 1))
    {}

    [[gnu::always_inline]]
    MatchResult add_order(Order* order) noexcept {
        MatchResult r{};
        uint32_t idx = order->id;

        if (order->type == OrderType::Limit) [[likely]] {
            if (order->side == Side::Buy)
                match_limit_buy(order, idx, r);
            else
                match_limit_sell(order, idx, r);
        } else {
            if (order->side == Side::Buy)
                match_market_buy(order, idx, r);
            else
                match_market_sell(order, idx, r);
        }

        if (!order->is_filled() && order->type == OrderType::Limit
            && order->status != OrderStatus::Cancelled) [[likely]] {
            if (order->filled_quantity > 0)
                order->status = OrderStatus::PartiallyFilled;
            level_at(order->price).push_back(idx, base_);
            ++resting_count_;
            if (order->side == Side::Buy) {
                if (order->price > best_bid_) best_bid_ = order->price;
            } else {
                if (order->price < best_ask_) best_ask_ = order->price;
            }
        } else if (order->is_filled()) {
            order->status = OrderStatus::Filled;
        } else if (order->type == OrderType::Market) {
            order->status = OrderStatus::Cancelled;
        }

        return r;
    }

    [[gnu::always_inline]]
    bool cancel_order(Order* order) noexcept {
        if (!order || order->status == OrderStatus::Filled
                   || order->status == OrderStatus::Cancelled)
            return false;

        Price p = order->price;
        auto& lv = level_at(p);
        lv.unlink(order->id, base_);
        --resting_count_;
        order->status = OrderStatus::Cancelled;

        if (lv.empty()) {
            if (order->side == Side::Buy  && p == best_bid_) advance_best_bid();
            if (order->side == Side::Sell && p == best_ask_) advance_best_ask();
        }
        return true;
    }

    Symbol   symbol()         const noexcept { return symbol_; }
    Price    best_bid()       const noexcept { return best_bid_; }
    Price    best_ask_raw()   const noexcept { return best_ask_; }
    Price    best_ask()       const noexcept {
        return best_ask_ > max_price_ ? 0 : best_ask_;
    }
    uint64_t trade_count()    const noexcept { return trade_count_; }
    uint64_t total_volume()   const noexcept { return total_volume_; }
    size_t   resting_orders() const noexcept { return resting_count_; }

    void prefetch_for_insert(Price p) noexcept {
        auto& lv = level_at(p);
        __builtin_prefetch(&lv, 1, 3);
        if (lv.tail_idx != NULL_IDX)
            __builtin_prefetch(&base_[lv.tail_idx], 1, 1);
    }

    void top_of_book(size_t depth,
                     std::vector<LevelInfo>& bids_out,
                     std::vector<LevelInfo>& asks_out) const;

private:
    PriceLevel& level_at(Price p) noexcept {
        return levels_[static_cast<size_t>(p - min_price_)];
    }

    [[gnu::always_inline]]
    void match_limit_buy(Order* inc, uint32_t /*inc_idx*/, MatchResult& r) noexcept {
        while (inc->remaining() > 0 && best_ask_ <= max_price_
               && inc->price >= best_ask_) [[likely]] {
            auto& lv = level_at(best_ask_);
            while (inc->remaining() > 0 && !lv.empty()) [[likely]] {
                Order& rest = base_[lv.head_idx];
                Quantity fill = std::min(inc->remaining(), rest.remaining());
                exec_fill(inc, &rest, fill, r);
                if (rest.is_filled()) [[likely]] {
                    lv.pop_front(base_);
                    rest.status = OrderStatus::Filled;
                    --resting_count_;
                } else {
                    rest.status = OrderStatus::PartiallyFilled;
                    lv.total_qty -= fill;
                }
            }
            if (lv.empty()) advance_best_ask();
        }
    }

    [[gnu::always_inline]]
    void match_limit_sell(Order* inc, uint32_t /*inc_idx*/, MatchResult& r) noexcept {
        while (inc->remaining() > 0 && best_bid_ >= min_price_
               && inc->price <= best_bid_) [[likely]] {
            auto& lv = level_at(best_bid_);
            while (inc->remaining() > 0 && !lv.empty()) [[likely]] {
                Order& rest = base_[lv.head_idx];
                Quantity fill = std::min(inc->remaining(), rest.remaining());
                exec_fill(&rest, inc, fill, r);
                if (rest.is_filled()) [[likely]] {
                    lv.pop_front(base_);
                    rest.status = OrderStatus::Filled;
                    --resting_count_;
                } else {
                    rest.status = OrderStatus::PartiallyFilled;
                    lv.total_qty -= fill;
                }
            }
            if (lv.empty()) advance_best_bid();
        }
    }

    void match_market_buy(Order* inc, uint32_t /*idx*/, MatchResult& r) noexcept {
        while (inc->remaining() > 0 && best_ask_ <= max_price_) {
            auto& lv = level_at(best_ask_);
            while (inc->remaining() > 0 && !lv.empty()) {
                Order& rest = base_[lv.head_idx];
                Quantity fill = std::min(inc->remaining(), rest.remaining());
                exec_fill(inc, &rest, fill, r);
                if (rest.is_filled()) {
                    lv.pop_front(base_);
                    rest.status = OrderStatus::Filled;
                    --resting_count_;
                } else {
                    rest.status = OrderStatus::PartiallyFilled;
                    lv.total_qty -= fill;
                }
            }
            if (lv.empty()) advance_best_ask();
        }
    }

    void match_market_sell(Order* inc, uint32_t /*idx*/, MatchResult& r) noexcept {
        while (inc->remaining() > 0 && best_bid_ >= min_price_) {
            auto& lv = level_at(best_bid_);
            while (inc->remaining() > 0 && !lv.empty()) {
                Order& rest = base_[lv.head_idx];
                Quantity fill = std::min(inc->remaining(), rest.remaining());
                exec_fill(&rest, inc, fill, r);
                if (rest.is_filled()) {
                    lv.pop_front(base_);
                    rest.status = OrderStatus::Filled;
                    --resting_count_;
                } else {
                    rest.status = OrderStatus::PartiallyFilled;
                    lv.total_qty -= fill;
                }
            }
            if (lv.empty()) advance_best_bid();
        }
    }

    [[gnu::always_inline]]
    void exec_fill(Order* buy, Order* sell, Quantity qty, MatchResult& r) noexcept {
        buy->filled_quantity  += qty;
        sell->filled_quantity += qty;
        ++r.num_trades;
        r.total_filled += qty;
        ++trade_count_;
        total_volume_ += qty;
    }

    void advance_best_ask() noexcept {
        while (best_ask_ <= max_price_ && level_at(best_ask_).empty())
            ++best_ask_;
    }
    void advance_best_bid() noexcept {
        while (best_bid_ >= min_price_ && level_at(best_bid_).empty())
            --best_bid_;
    }

    Symbol  symbol_;
    Price   min_price_;
    Price   max_price_;
    Price   best_bid_;
    Price   best_ask_;
    Order*  base_;

    size_t   resting_count_ = 0;
    uint64_t trade_count_   = 0;
    uint64_t total_volume_  = 0;

    std::vector<PriceLevel> levels_;
};

}
