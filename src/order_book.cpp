#include "order_book.hpp"

namespace ome {

void FlatOrderBook::top_of_book(size_t depth,
                                 std::vector<LevelInfo>& bids_out,
                                 std::vector<LevelInfo>& asks_out) const
{
    bids_out.clear();
    asks_out.clear();

    for (Price p = best_bid_; p >= min_price_ && bids_out.size() < depth; --p) {
        auto& lv = levels_[static_cast<size_t>(p - min_price_)];
        if (!lv.empty())
            bids_out.push_back({p, lv.total_qty, lv.count});
    }
    for (Price p = best_ask_; p <= max_price_ && asks_out.size() < depth; ++p) {
        auto& lv = levels_[static_cast<size_t>(p - min_price_)];
        if (!lv.empty())
            asks_out.push_back({p, lv.total_qty, lv.count});
    }
}

} // namespace ome
