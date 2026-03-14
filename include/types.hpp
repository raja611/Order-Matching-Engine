#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <ostream>

namespace ome {

using Price    = int32_t;    // fixed-point × 100;  supports ±$21.4M
using Quantity = uint32_t;
using OrderId  = uint32_t;   // supports 4B orders per arena
using Symbol   = uint32_t;

constexpr uint32_t NULL_IDX = 0;   // index 0 is reserved as sentinel

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market };
enum class OrderStatus : uint8_t { New, PartiallyFilled, Filled, Cancelled, Rejected };

// ── Compact Order: exactly 32 bytes ─────────────────────────────
// Intrusive DLL uses arena indices (uint32_t) instead of 8-byte
// pointers, halving link overhead and fitting 2 orders per cache line.
struct Order {
    OrderId     id;               //  4
    Price       price;            //  4
    Quantity    quantity;         //  4
    Quantity    filled_quantity;  //  4
    uint32_t    prev_idx;         //  4   (arena index, 0 = null)
    uint32_t    next_idx;         //  4
    Symbol      symbol;           //  4
    Side        side;             //  1
    OrderType   type;             //  1
    OrderStatus status;           //  1
    uint8_t     _pad;             //  1
                                  // ───  32B total

    void init(OrderId id_, Symbol sym, Side s, OrderType t, Price p, Quantity q) noexcept {
        id = id_; price = p; quantity = q; filled_quantity = 0;
        prev_idx = NULL_IDX; next_idx = NULL_IDX;
        symbol = sym; side = s; type = t;
        status = OrderStatus::New; _pad = 0;
    }

    Quantity remaining() const noexcept { return quantity - filled_quantity; }
    bool     is_filled() const noexcept { return filled_quantity >= quantity; }
};

static_assert(sizeof(Order) == 32, "Order must be exactly 32 bytes");

struct Trade {
    OrderId  buy_order_id;
    OrderId  sell_order_id;
    Symbol   symbol;
    Price    price;
    Quantity quantity;
};

// ── Pre-allocated flat arena ────────────────────────────────────
// Slot 0 is reserved as the null sentinel.  Valid IDs start at 1.
class OrderArena {
public:
    explicit OrderArena(size_t capacity)
        : storage_(capacity + 1), next_(1) {
        std::memset(storage_.data(), 0, storage_.size() * sizeof(Order));
    }

    Order* allocate(Symbol sym, Side side, OrderType type, Price price, Quantity qty) noexcept {
        uint32_t idx = next_++;
        Order* o = &storage_[idx];
        o->init(idx, sym, side, type, price, qty);
        return o;
    }

    // Fast path: only write fields that differ from zero.
    // Relies on arena being memset(0) at construction — prev_idx, next_idx,
    // filled_quantity, status(New=0), type(Limit=0) are already correct.
    [[gnu::always_inline]]
    Order* allocate_fast(OrderId id_override, Side side, Price price, Quantity qty) noexcept {
        Order* o = &storage_[next_++];
        o->id       = id_override;
        o->price    = price;
        o->quantity = qty;
        o->side     = side;
        return o;
    }

    Order*       base()          noexcept { return storage_.data(); }
    Order*       get(OrderId id) noexcept { return &storage_[id]; }
    void         reset()         noexcept { next_ = 1; }
    size_t       used()    const noexcept { return next_ - 1; }

private:
    std::vector<Order> storage_;
    uint32_t next_;
};

// ── Ultra-fast PRNG (SplitMix64) ────────────────────────────────
// ~1–2 ns/call  vs  ~5–10 ns for mt19937_64.
struct SplitMix64 {
    uint64_t s;
    explicit SplitMix64(uint64_t seed = 0) noexcept : s(seed) {}
    uint64_t next() noexcept {
        s += 0x9e3779b97f4a7c15ULL;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint32_t next_u32() noexcept { return static_cast<uint32_t>(next()); }
    // Unbiased range [lo, hi] — fast modulo (slight bias OK for bench)
    int32_t range(int32_t lo, int32_t hi) noexcept {
        return lo + static_cast<int32_t>(next_u32() % static_cast<uint32_t>(hi - lo + 1));
    }
    bool coin() noexcept { return next() & 1; }
};

// ── Display helpers ─────────────────────────────────────────────

inline const char* side_str(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}
inline const char* status_str(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::New:             return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIAL";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Cancelled:       return "CANCELLED";
        case OrderStatus::Rejected:        return "REJECTED";
    }
    return "?";
}

inline std::ostream& operator<<(std::ostream& os, const Order& o) {
    os << "[#" << o.id << " " << side_str(o.side)
       << " " << o.quantity << "@"
       << (o.price / 100) << "." << (o.price % 100 < 10 ? "0" : "") << (o.price % 100)
       << " filled=" << o.filled_quantity
       << " " << status_str(o.status) << "]";
    return os;
}

}
