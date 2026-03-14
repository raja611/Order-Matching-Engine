# Code Explanation: Line-by-Line Walkthrough

A complete technical explanation of every file, every struct, every function,
and every design choice in the optimised order matching engine — from the
foundational concepts of exchange matching through to the final 26 M orders/sec
implementation.

---

## Table of Contents

- [1. What Is an Order Matching Engine?](#1-what-is-an-order-matching-engine)
- [2. Project Layout](#2-project-layout)
- [3. `include/types.hpp` — Foundational Types](#3-includetypeshpp--foundational-types)
  - [3.1 Type Aliases](#31-type-aliases)
  - [3.2 Enums](#32-enums)
  - [3.3 The 32-Byte Order Struct](#33-the-32-byte-order-struct)
  - [3.4 The Trade Struct](#34-the-trade-struct)
  - [3.5 OrderArena — Zero-Malloc Allocator](#35-orderarena--zero-malloc-allocator)
  - [3.6 SplitMix64 — Ultra-Fast PRNG](#36-splitmix64--ultra-fast-prng)
  - [3.7 Display Helpers](#37-display-helpers)
- [4. `include/order_book.hpp` — The Heart of the Engine](#4-includeorder_bookhpp--the-heart-of-the-engine)
  - [4.1 PriceLevel — 16-Byte Intrusive Linked List](#41-pricelevel--16-byte-intrusive-linked-list)
  - [4.2 FlatOrderBook — The Matching Engine Core](#42-flatorderbook--the-matching-engine-core)
  - [4.3 add_order — The Hot Path](#43-add_order--the-hot-path)
  - [4.4 match_limit_buy — Price-Time Priority Matching](#44-match_limit_buy--price-time-priority-matching)
  - [4.5 match_limit_sell — The Mirror](#45-match_limit_sell--the-mirror)
  - [4.6 Market Order Matching](#46-market-order-matching)
  - [4.7 exec_fill — Trade Execution](#47-exec_fill--trade-execution)
  - [4.8 cancel_order — O(1) Cancellation](#48-cancel_order--o1-cancellation)
  - [4.9 Best-Price Scanning](#49-best-price-scanning)
  - [4.10 Prefetch Support](#410-prefetch-support)
- [5. `src/order_book.cpp` — Cold Path](#5-srcorder_bookcpp--cold-path)
- [6. `include/matching_engine.hpp` — Engine Facade](#6-includematching_enginehpp--engine-facade)
- [7. `src/main.cpp` — Demo Driver](#7-srcmaincpp--demo-driver)
- [8. `bench/benchmark.cpp` — Performance Measurement](#8-benchbenchmarkcpp--performance-measurement)
- [9. `Makefile` — Build System](#9-makefile--build-system)
- [10. How It All Fits Together — Request Flow](#10-how-it-all-fits-together--request-flow)

---

## 1. What Is an Order Matching Engine?

An order matching engine is the core software of a financial exchange. It
receives buy and sell orders from traders and matches them according to rules.

**Central Limit Order Book (CLOB):**

```
         BIDS (buyers)          ASKS (sellers)
    ───────────────────    ───────────────────
    150 shares @ $149.75   50  shares @ $150.25   ← best ask
    250 shares @ $149.50   100 shares @ $150.50
    500 shares @ $149.00   200 shares @ $150.75
         ↑ best bid              ↑ spread
```

**Matching Rules (Price-Time Priority):**

1. **Price Priority:** A buy at $150.25 matches against the lowest available
   sell price first. A sell at $149.75 matches against the highest buy first.
2. **Time Priority:** Among orders at the same price, the one that arrived
   first gets filled first (FIFO — first in, first out).
3. **Limit Orders** rest in the book if they can't match immediately.
4. **Market Orders** match against whatever is available at any price.

**Example match:** If a new BUY 80 @ $150.25 arrives:
- The best ask is 50 shares at $150.25
- The buy's price ($150.25) >= the ask's price ($150.25) → match!
- 50 shares fill at $150.25 (the resting order's price)
- The remaining 30 shares of the buy rest in the book as a new bid at $150.25

---

## 2. Project Layout

```
order-matching-engine/
├── include/                     ← Header files (all hot-path code lives here)
│   ├── types.hpp                ← Order, Trade, OrderArena, SplitMix64, enums
│   ├── order_book.hpp           ← PriceLevel, FlatOrderBook (matching logic)
│   └── matching_engine.hpp      ← MatchingEngine (multi-symbol routing, stats)
├── src/                         ← Source files
│   ├── order_book.cpp           ← Cold-path only (display function)
│   ├── matching_engine.cpp      ← Compilation stub (engine is header-only)
│   └── main.cpp                 ← Interactive demo
├── bench/
│   └── benchmark.cpp            ← Throughput, latency, cancel, multi-thread benchmarks
├── Makefile                     ← Build: release, debug, PGO targets
├── CMakeLists.txt               ← Alternative CMake build
├── README.md                    ← Project overview and results
├── OPTIMIZATION_JOURNEY.md      ← Step-by-step optimisation record
└── CODE_EXPLANATION.md          ← This file
```

**Why the hot path is in headers:** The compiler can only inline function
calls within the same compilation unit (translation unit). By putting all
matching logic in headers, we guarantee the compiler can inline the entire
`add_order → match_limit_buy → exec_fill` chain into a single function
with zero call overhead. The `[[gnu::always_inline]]` attribute makes this
mandatory rather than advisory.

---

## 3. `include/types.hpp` — Foundational Types

This file defines the data types that every other file depends on.

### 3.1 Type Aliases

```cpp
using Price    = int32_t;    // fixed-point × 100;  supports ±$21.4M
using Quantity = uint32_t;
using OrderId  = uint32_t;   // supports 4B orders per arena
using Symbol   = uint32_t;
```

**Why `int32_t` instead of `double` for Price:**
Floating-point has rounding errors. `0.1 + 0.2 != 0.3` in IEEE 754.
For financial math, we use fixed-point: store the price in cents as an integer.
`$150.25` becomes `15025`. Integer comparison is exact and takes 1 CPU cycle
vs ~3-5 cycles for floating-point.

**Why `int32_t` instead of `int64_t`:**
`int32_t` holds ±2,147,483,647 → ±$21,474,836.47 in cents. Sufficient for
any stock price. Using 32-bit types instead of 64-bit saves 4 bytes per
field, which lets us fit the entire `Order` struct in 32 bytes (half a
64-byte cache line).

**Why `uint32_t OrderId`:**
Supports up to 4 billion orders per arena. The ID doubles as the index
into the pre-allocated arena array, so `arena.get(id)` is a single
array access — no hash table needed.

### 3.2 Enums

```cpp
constexpr uint32_t NULL_IDX = 0;   // index 0 is reserved as sentinel

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market };
enum class OrderStatus : uint8_t { New, PartiallyFilled, Filled, Cancelled, Rejected };
```

**Why `: uint8_t`?** By default, enums use `int` (4 bytes). Specifying
`uint8_t` makes each enum 1 byte, saving space in the `Order` struct.
Every byte saved in `Order` matters — it's the most-accessed structure
in the entire engine.

**Why `NULL_IDX = 0`?** Arena slot 0 is reserved and never used for a real
order. This lets us use `0` as "null pointer equivalent" for linked-list
terminators. When the arena is `memset` to zero at construction, all
`prev_idx` and `next_idx` fields start as `NULL_IDX` automatically — no
explicit initialisation needed on the fast path.

### 3.3 The 32-Byte Order Struct

```cpp
struct Order {
    OrderId     id;               //  4   — also the arena index
    Price       price;            //  4   — fixed-point cents
    Quantity    quantity;          //  4   — total shares requested
    Quantity    filled_quantity;   //  4   — shares matched so far
    uint32_t    prev_idx;         //  4   — previous order at same price level
    uint32_t    next_idx;         //  4   — next order at same price level
    Symbol      symbol;           //  4   — which instrument (e.g. AAPL)
    Side        side;             //  1   — Buy or Sell
    OrderType   type;             //  1   — Limit or Market
    OrderStatus status;           //  1   — New/Partial/Filled/Cancelled
    uint8_t     _pad;             //  1   — explicit padding to reach 32
};                                // ───── 32 total

static_assert(sizeof(Order) == 32, "Order must be exactly 32 bytes");
```

**The 32-byte target:** A CPU cache line is 64 bytes. By making each `Order`
exactly 32 bytes, we fit **2 orders per cache line**. When the matching
engine reads the head of a price level's queue, it gets the next order
for free in the same cache line fetch.

**Field ordering matters:** Fields are ordered so that the most frequently
accessed fields (`id`, `price`, `quantity`, `filled_quantity`) are in the
first 16 bytes — the first half of a cache line subdivision. The less
frequently accessed fields (`symbol`, `side`, `type`, `status`) are in
the second half.

**`prev_idx` / `next_idx` — The Intrusive Linked List:**
Instead of storing 8-byte `Order*` pointers, we store 4-byte arena indices.
To follow a link: `Order& next_order = arena_base[this->next_idx]`.
This pointer arithmetic (base + index × 32) takes 1 cycle. The 4-byte
saving per pointer (×2 pointers) reclaims 8 bytes — the difference
between a 40-byte struct and the 32-byte target.

**`remaining()` and `is_filled()`:** These helper methods are called on
every matching iteration. `remaining()` computes `quantity - filled_quantity`
— the shares still waiting to be matched. `is_filled()` checks if the
order is complete. Both are `noexcept` to help the compiler optimise.

### 3.4 The Trade Struct

```cpp
struct Trade {
    OrderId  buy_order_id;
    OrderId  sell_order_id;
    Symbol   symbol;
    Price    price;
    Quantity quantity;
};
```

A `Trade` records that a match occurred. In the optimised engine, trades
are not constructed on the hot path — only counters are incremented. The
`Trade` struct exists for the engine API (e.g., for external trade reporting).

### 3.5 OrderArena — Zero-Malloc Allocator

```cpp
class OrderArena {
public:
    explicit OrderArena(size_t capacity)
        : storage_(capacity + 1), next_(1) {
        std::memset(storage_.data(), 0, storage_.size() * sizeof(Order));
    }
```

**Construction:**
- Allocates `capacity + 1` order slots as a contiguous `std::vector<Order>`.
  Slot 0 is the null sentinel; real orders start at index 1.
- `memset` zeros the entire arena. This means every `Order` starts with
  `prev_idx=0` (`NULL_IDX`), `next_idx=0`, `filled_quantity=0`,
  `status=New(0)`, `type=Limit(0)`. These are all correct default values.

**`allocate()` — Standard path (used by MatchingEngine):**
```cpp
Order* allocate(Symbol sym, Side side, OrderType type,
                Price price, Quantity qty) noexcept {
    uint32_t idx = next_++;           // bump the slot counter
    Order* o = &storage_[idx];        // index into the flat array
    o->init(idx, sym, side, type, price, qty);  // write all fields
    return o;
}
```
`O(1)` — no heap allocation, no free-list traversal. Just increment a
counter and write to the next slot in a pre-allocated array.

**`allocate_fast()` — Benchmark fast path:**
```cpp
[[gnu::always_inline]]
Order* allocate_fast(OrderId id_override, Side side, Price price,
                     Quantity qty) noexcept {
    Order* o = &storage_[next_++];
    o->id       = id_override;        // 4 bytes
    o->price    = price;              // 4 bytes
    o->quantity = qty;                // 4 bytes
    o->side     = side;               // 1 byte
    return o;                         // skip 8 redundant zero-writes
}
```
Only writes the 4 fields that differ from zero. Saves ~8 store instructions
per order compared to `allocate()`. The `[[gnu::always_inline]]` attribute
forces GCC to inline this into the caller — no function-call overhead.

**`get()` — O(1) order lookup:**
```cpp
Order* get(OrderId id) noexcept { return &storage_[id]; }
```
Since `OrderId == arena index`, looking up an order is a single array
access: `base + id × 32 bytes`. This replaces `std::unordered_map` which
would require hashing + bucket chain traversal.

### 3.6 SplitMix64 — Ultra-Fast PRNG

```cpp
struct SplitMix64 {
    uint64_t s;     // 8 bytes of state — fits in a single CPU register

    uint64_t next() noexcept {
        s += 0x9e3779b97f4a7c15ULL;      // golden ratio constant
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;  // avalanche mix
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
};
```

**Why not `std::mt19937_64`?** The Mersenne Twister maintains 2,496 bytes
of state (312 `uint64_t` values). Reading and updating this state on every
call touches 5+ cache lines. SplitMix64 has 8 bytes of state — it lives
in a single CPU register and never touches memory. At ~1.5 ns/call vs
~5-10 ns for mt19937, it removes ~10 ns of overhead per order in benchmarks.

**How it works:** The golden ratio constant `0x9e3779b97f4a7c15` ensures
the state advances uniformly. The xor-shift-multiply sequence (avalanche
mixing) ensures all output bits depend on all input bits, producing
high-quality pseudo-random numbers that pass the BigCrush statistical
test suite.

### 3.7 Display Helpers

```cpp
inline const char* side_str(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}
```

Simple string conversion for logging. The `inline` keyword prevents
linker errors when the header is included from multiple `.cpp` files.

---

## 4. `include/order_book.hpp` — The Heart of the Engine

This single header contains the entire performance-critical matching
engine. Every function that runs on the hot path is defined here so
the compiler can inline aggressively.

### 4.1 PriceLevel — 16-Byte Intrusive Linked List

A PriceLevel represents all orders at a single price (e.g., all bids at
$149.75). Orders are stored in arrival order (FIFO) using an intrusive
doubly-linked list.

```cpp
struct PriceLevel {
    uint32_t head_idx = NULL_IDX;   // first order in the queue (oldest)
    uint32_t tail_idx = NULL_IDX;   // last order in the queue (newest)
    Quantity total_qty = 0;         // sum of remaining() across all orders
    uint32_t count     = 0;         // number of orders at this level
};                                  // 16 bytes total

static_assert(sizeof(PriceLevel) == 16, "PriceLevel must be 16 bytes");
```

**Why 16 bytes matters:** For a price range of $90.00–$110.00 (2,001 tick
levels), the levels array is `2001 × 16 = 31 KB`. This fits entirely in
the CPU's L1 data cache (typically 32-48 KB). Every level access is an
L1 cache hit (~1 ns). If each level were 24 bytes (using 8-byte pointers),
the array would be 48 KB — potentially spilling into the slower L2 cache.

**`push_back()` — Append an order to the tail:**
```cpp
void push_back(uint32_t idx, Order* base) noexcept {
    Order& o = base[idx];              // resolve arena index to pointer
    o.prev_idx = tail_idx;             // new order's prev = current tail
    o.next_idx = NULL_IDX;             // new order is the new tail (no next)
    if (tail_idx != NULL_IDX)
        base[tail_idx].next_idx = idx; // old tail points forward to new order
    else
        head_idx = idx;                // empty list: new order is also the head
    tail_idx = idx;                    // update tail
    total_qty += o.remaining();        // update aggregate quantity
    ++count;
}
```

This is a standard doubly-linked list append, but using arena indices
instead of raw pointers. The `base` parameter is the arena's base address;
`base[idx]` converts an index to a reference.

**`unlink()` — Remove any order from anywhere in the list (O(1)):**
```cpp
void unlink(uint32_t idx, Order* base) noexcept {
    Order& o = base[idx];
    if (o.prev_idx != NULL_IDX) base[o.prev_idx].next_idx = o.next_idx;
    else                         head_idx = o.next_idx;
    if (o.next_idx != NULL_IDX) base[o.next_idx].prev_idx = o.prev_idx;
    else                         tail_idx = o.prev_idx;
    total_qty -= o.remaining();
    --count;
}
```

This is the key advantage of an intrusive doubly-linked list: given just
the order's index, we can remove it in O(1) by relinking its neighbours.
The previous implementation used `std::list::remove()` which did a **linear
scan** — O(N) per cancel. With thousands of orders per level, this was
the dominant bottleneck for cancel operations (3,424 ns → 15.8 ns after
this change, a 217× improvement).

**`pop_front()` — Remove the oldest order (used during matching):**
```cpp
uint32_t pop_front(Order* base) noexcept {
    uint32_t idx = head_idx;
    Order& o = base[idx];
    head_idx = o.next_idx;               // advance head to next order
    if (head_idx != NULL_IDX)
        base[head_idx].prev_idx = NULL_IDX;  // new head has no predecessor
    else
        tail_idx = NULL_IDX;             // list is now empty
    total_qty -= o.remaining();
    --count;
    return idx;
}
```

### 4.2 FlatOrderBook — The Matching Engine Core

```cpp
class FlatOrderBook {
    // ...
    FlatOrderBook(Symbol symbol, Price min_price, Price max_price, Order* base)
        : symbol_(symbol), min_price_(min_price), max_price_(max_price),
          best_bid_(min_price - 1),     // no bids yet: below valid range
          best_ask_(max_price + 1),     // no asks yet: above valid range
          base_(base),                  // pointer to arena's order storage
          levels_(static_cast<size_t>(max_price - min_price + 1))
    {}
```

**The flat array of price levels:**
```cpp
std::vector<PriceLevel> levels_;   // indexed by (price - min_price)

PriceLevel& level_at(Price p) noexcept {
    return levels_[static_cast<size_t>(p - min_price_)];
}
```

If `min_price = 9000` and `max_price = 11000`, the array has 2,001
entries. To access the level for price 10050: `levels_[10050 - 9000]`
= `levels_[1050]`. This is a single array index — **O(1)** with no
comparison, no tree traversal, no hash computation.

**The `base_` pointer:** All `Order` objects live in the external
`OrderArena`. The book stores `base_` — the arena's base address —
and uses it in every linked-list operation to resolve indices to pointers.

**`best_bid_` / `best_ask_`:** These scalar variables track the
highest occupied bid price and lowest occupied ask price. When a new
order arrives, the matching logic first checks if the order's price
crosses `best_bid_` or `best_ask_` — a single integer comparison.
If it doesn't cross, the order rests in the book with no matching
work at all.

### 4.3 add_order — The Hot Path

This is the single most important function in the engine. Every order
passes through it.

```cpp
[[gnu::always_inline]]
MatchResult add_order(Order* order) noexcept {
    MatchResult r{};                   // {num_trades=0, total_filled=0}
    uint32_t idx = order->id;          // id == arena index (used for DLL)
```

**Step 1: Dispatch to the correct matching function.**
```cpp
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
```

`[[likely]]` tells the compiler that most orders are limit orders,
so it should arrange the code with the limit path as straight-line
(no jump) and the market path out-of-line (a taken branch). This
improves instruction-cache locality.

**Step 2: Rest unfilled limit orders in the book.**
```cpp
    if (!order->is_filled() && order->type == OrderType::Limit
        && order->status != OrderStatus::Cancelled) [[likely]] {
        if (order->filled_quantity > 0)
            order->status = OrderStatus::PartiallyFilled;
        level_at(order->price).push_back(idx, base_);
        ++resting_count_;
```

If the order has remaining quantity after matching, insert it into
the book at its price level. `level_at(order->price)` is a single
array access, and `push_back()` appends to the intrusive linked list.

**Step 3: Update the best bid/ask if this order improves it.**
```cpp
        if (order->side == Side::Buy) {
            if (order->price > best_bid_) best_bid_ = order->price;
        } else {
            if (order->price < best_ask_) best_ask_ = order->price;
        }
```

Simple scalar comparison. If a new buy at $150.50 arrives and the
current `best_bid_` is $150.00, the best bid becomes $150.50.

### 4.4 match_limit_buy — Price-Time Priority Matching

This function implements the core matching algorithm for an incoming
buy limit order.

```cpp
[[gnu::always_inline]]
void match_limit_buy(Order* inc, uint32_t /*inc_idx*/, MatchResult& r) noexcept {
```

**Outer loop — iterate through ask price levels:**
```cpp
    while (inc->remaining() > 0           // incoming still has quantity
           && best_ask_ <= max_price_     // there are asks in the book
           && inc->price >= best_ask_)    // price is acceptable
    [[likely]] {
        auto& lv = level_at(best_ask_);   // O(1) array access
```

The condition `inc->price >= best_ask_` is the **price priority** check.
A buy at $150.25 can match against asks at $150.25 or lower, but not
against asks at $150.50 or higher.

**Inner loop — match against orders at this price level (FIFO):**
```cpp
        while (inc->remaining() > 0 && !lv.empty()) [[likely]] {
            Order& rest = base_[lv.head_idx];  // oldest order (time priority)
            Quantity fill = std::min(inc->remaining(), rest.remaining());
            exec_fill(inc, &rest, fill, r);    // execute the trade
```

`lv.head_idx` is the oldest order at this price — **time priority**.
The fill quantity is the minimum of what the incoming order wants and
what the resting order has.

**After filling, handle the resting order's fate:**
```cpp
            if (rest.is_filled()) [[likely]] {
                lv.pop_front(base_);           // remove from the queue
                rest.status = OrderStatus::Filled;
                --resting_count_;
            } else {
                rest.status = OrderStatus::PartiallyFilled;
                lv.total_qty -= fill;          // decrement aggregate qty
            }
        }
```

If the resting order is fully consumed, `pop_front()` removes it from
the intrusive linked list. If partially filled, it remains at the head
of the queue (maintaining time priority for its remaining quantity).

**Level exhausted — advance to the next ask price:**
```cpp
        if (lv.empty()) advance_best_ask();
    }
}
```

When all orders at the best ask are consumed, `advance_best_ask()`
scans upward to find the next occupied ask level.

### 4.5 match_limit_sell — The Mirror

```cpp
void match_limit_sell(Order* inc, uint32_t /*inc_idx*/, MatchResult& r) noexcept {
    while (inc->remaining() > 0 && best_bid_ >= min_price_
           && inc->price <= best_bid_) [[likely]] {
```

Identical logic but mirrored: a sell matches against **bids** (highest
first), and the condition is `inc->price <= best_bid_` (a sell at $149.50
can match bids at $149.50 or higher). Fills are reported with the bid
order as `buy` and the incoming order as `sell`.

### 4.6 Market Order Matching

```cpp
void match_market_buy(Order* inc, uint32_t /*idx*/, MatchResult& r) noexcept {
    while (inc->remaining() > 0 && best_ask_ <= max_price_) {
```

Market orders have no price limit — they match against **any** available
price. The only stopping condition is either the incoming order is fully
filled, or the book is empty. There is no `inc->price >= best_ask_`
check because market orders accept any price.

### 4.7 exec_fill — Trade Execution

```cpp
[[gnu::always_inline]]
void exec_fill(Order* buy, Order* sell, Quantity qty, MatchResult& r) noexcept {
    buy->filled_quantity  += qty;      // update buyer's progress
    sell->filled_quantity += qty;      // update seller's progress
    ++r.num_trades;                    // returned to caller
    r.total_filled += qty;             // returned to caller
    ++trade_count_;                    // book-level statistic
    total_volume_ += qty;              // book-level statistic
}
```

**Why no Trade object is constructed:** In the baseline implementation,
every fill created a `Trade` struct and invoked a `std::function` callback.
That involved: allocating a `Trade` on the stack (20 bytes), calling
`steady_clock::now()` for a timestamp (a kernel syscall), and executing
an indirect function call through the type-erased `std::function` wrapper.

The optimised version replaces all of that with 6 integer increments —
single-cycle operations that the CPU can execute in parallel.

### 4.8 cancel_order — O(1) Cancellation

```cpp
[[gnu::always_inline]]
bool cancel_order(Order* order) noexcept {
    if (!order || order->status == OrderStatus::Filled
               || order->status == OrderStatus::Cancelled)
        return false;

    Price p = order->price;
    auto& lv = level_at(p);            // O(1) array access
    lv.unlink(order->id, base_);       // O(1) doubly-linked list removal
    --resting_count_;
    order->status = OrderStatus::Cancelled;
```

Given a pointer to the order (obtained via `arena.get(id)`), cancellation
is O(1):
1. `level_at(p)` — array index: 1 cycle
2. `lv.unlink()` — relink 2-4 pointers: ~4 cycles
3. Update counters: ~2 cycles

**Best-price recalculation if the level is now empty:**
```cpp
    if (lv.empty()) {
        if (order->side == Side::Buy  && p == best_bid_) advance_best_bid();
        if (order->side == Side::Sell && p == best_ask_) advance_best_ask();
    }
    return true;
}
```

Only recalculates if the cancelled order was at the best bid/ask AND
the level is now empty — both of which are uncommon in a liquid market.

### 4.9 Best-Price Scanning

```cpp
void advance_best_ask() noexcept {
    while (best_ask_ <= max_price_ && level_at(best_ask_).empty())
        ++best_ask_;
}
```

After exhausting all orders at the best ask, scan upward through the
flat array to find the next non-empty level. In a liquid market with
tight spreads, this loop typically executes 1-2 iterations (adjacent
ticks are occupied). Worst case is O(price_range), but with 2K levels
all in L1 cache, even a full scan takes microseconds.

### 4.10 Prefetch Support

```cpp
void prefetch_for_insert(Price p) noexcept {
    auto& lv = level_at(p);
    __builtin_prefetch(&lv, 1, 3);           // bring level into L1 cache
    if (lv.tail_idx != NULL_IDX)
        __builtin_prefetch(&base_[lv.tail_idx], 1, 1);  // bring tail order into L2
}
```

`__builtin_prefetch(addr, write, locality)` issues a hardware prefetch
instruction. Parameters:
- `write=1`: hint that we intend to write (prefetch into the "modified" cache state)
- `locality=3`: high temporal locality (keep in L1)
- `locality=1`: moderate locality (keep in L2)

This is used in the software-pipelined benchmark loop where order N+1's
target level is prefetched while order N is being processed. The technique
helps when levels are in L2/L3, but showed **no benefit** when levels
already fit in L1 (narrow price ranges).

---

## 5. `src/order_book.cpp` — Cold Path

```cpp
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
    // ...similar for asks, scanning upward from best_ask_...
}
```

This is the only function in a `.cpp` file. It scans the flat array
starting from `best_bid_` (downward) and `best_ask_` (upward) to
collect the top N price levels for display. It's called only for
`print_book()` — never on the hot path — so cross-TU overhead is irrelevant.

---

## 6. `include/matching_engine.hpp` — Engine Facade

The `MatchingEngine` is the user-facing API. It manages multiple symbols,
routes orders to the correct `FlatOrderBook`, and tracks aggregate statistics.

**Core architecture:**
```cpp
class MatchingEngine {
    std::unordered_map<Symbol, std::unique_ptr<FlatOrderBook>> books_;
    OrderArena arena_;
    Stats      stats_;
};
```

- `books_`: maps a symbol ID to its order book. Uses `unordered_map` for
  O(1) lookup (fine here because symbol lookup happens once per order,
  and there are typically few symbols).
- `arena_`: single pre-allocated arena shared by all books.
- `stats_`: aggregate counters (submitted, filled, cancelled, etc.).

**`submit_order()` — The user-facing entry point:**
```cpp
OrderId submit_order(Symbol sym, Side side, OrderType type,
                     Price price, Quantity quantity) noexcept
{
    auto it = books_.find(sym);
    if (__builtin_expect(it == books_.end(), 0)) {  // branch hint: very unlikely
        ++stats_.orders_rejected;
        return 0;
    }

    Order* o = arena_.allocate(sym, side, type, price, quantity);
    ++stats_.orders_submitted;

    auto r = it->second->add_order(o);     // → FlatOrderBook::add_order()
    stats_.trades_executed += r.num_trades;
    stats_.total_volume   += r.total_filled;

    if (o->is_filled())              ++stats_.orders_filled;
    else if (o->filled_quantity > 0) ++stats_.orders_partial;

    return o->id;
}
```

`__builtin_expect(expr, 0)` tells GCC that the expression is almost always
false. The compiler puts the error-handling path out-of-line, keeping the
common path (valid symbol → allocate → match) as compact sequential code.

**`cancel_order()`:**
```cpp
bool cancel_order(Symbol sym, OrderId id) noexcept {
    auto it = books_.find(sym);
    if (it == books_.end()) return false;
    bool ok = it->second->cancel_order(arena_.get(id));
    // arena_.get(id) → &storage_[id] → O(1) lookup by OrderId
    if (ok) ++stats_.orders_cancelled;
    return ok;
}
```

Lookup the order by ID (single array access into the arena), then
delegate to `FlatOrderBook::cancel_order()` which unlinks it in O(1).

---

## 7. `src/main.cpp` — Demo Driver

The demo creates a single-symbol book (AAPL) and runs through several
matching scenarios:

```cpp
MatchingEngine engine(100'000);        // arena capacity: 100K orders
constexpr Symbol AAPL = 1;
engine.add_symbol(AAPL, 14000, 16000); // price range $140.00–$160.00
```

**Scenario 1: Build the book.** Places 4 sell and 4 buy orders that
don't cross (bids below $150, asks above $150). These all rest in the book.

**Scenario 2: Aggressive buy crosses the spread.** BUY 80 @ $150.25
matches against the resting SELL 50 @ $150.25. 50 shares fill; the
remaining 30 shares rest as a new bid at $150.25.

**Scenario 3: Market order sweeps.** A market BUY 200 takes whatever
ask liquidity is available, sweeping across multiple price levels.

**Scenario 4: Cancel.** Places a sell order, prints the book (order
visible), cancels it, prints again (order gone).

**Scenario 5: Multi-level sweep.** SELL 500 @ $149.00 sweeps through
bids at $150.25, $150.00, $149.75, and partially at $149.50.

---

## 8. `bench/benchmark.cpp` — Performance Measurement

The benchmark suite measures six aspects of engine performance.

### Utility Functions

```cpp
static void pin_to_core(int core) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}
```

`pthread_setaffinity_np` binds the calling thread to a specific CPU core.
Without this, the OS can migrate the thread between cores, causing L1/L2
cache flushes (~1-10 μs latency spike each time).

### Benchmark 1: Single-Thread Matching Throughput

```cpp
static void bench_raw(size_t N) {
    pin_to_core(0);
    OrderArena arena(N + 2);
    FlatOrderBook book(1, MIN_P, MAX_P, arena.base());
    SplitMix64 rng(42);

    auto t0 = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        uint64_t r = rng.next();                         // 1 RNG call (1.5 ns)
        Side  s = (r & 1) ? Side::Buy : Side::Sell;      // bit 0
        Price p = 9900 + static_cast<int32_t>((r >> 1) % 201);  // bits 1-8
        Quantity q = 1 + static_cast<Quantity>((r >> 9) % 100);  // bits 9-16
        Order* o = arena.allocate_fast(...);              // 4 field writes
        book.add_order(o);                                // matching + insert
    }
    auto t1 = Clock::now();
```

**Key design choices:**
- Single `rng.next()` call extracts side, price, and quantity from
  different bit ranges of one 64-bit value. This halves RNG overhead
  compared to 3 separate calls.
- `allocate_fast()` skips redundant zero-writes.
- No pre-generated command array — generating inline avoids a 120 MB
  vector that would pollute the cache.

### Benchmark 2: Insert-Only Throughput

```cpp
static void bench_insert(const char* tag, size_t N,
                          Price buy_lo, Price buy_hi,
                          Price sell_lo, Price sell_hi) {
```

Uses a **wide spread**: buys at $90-$95, sells at $105-$110. Since bids
and asks never overlap, **zero matching occurs** — every order rests in
the book. This isolates the pure insertion throughput from matching overhead.

The "narrow" variant (500 ticks per side, ~16 KB of levels) fits entirely
in L1 cache and achieves the highest throughput: **77 M orders/sec**.

### Benchmark 3: Software-Pipelined Prefetch

```cpp
// Prime: generate first order
Order* cur = arena.allocate_fast(...);

for (size_t i = 1; i < N; ++i) {
    // Generate NEXT order and prefetch its target level
    Order* nxt = arena.allocate_fast(...);
    book.prefetch_for_insert(g.price);    // issue hardware prefetch

    // Process CURRENT order (level may already be in cache from prefetch)
    book.add_order(cur);
    cur = nxt;
}
book.add_order(cur);  // process the last one
```

This loop overlaps order N+1's prefetch with order N's processing.
By the time `add_order(nxt)` runs on the next iteration, the target
level's cache line should already be loaded from L2/L3 into L1.

**Result:** No benefit when levels fit in L1 (narrow range). The
prefetch instruction itself costs ~2 cycles of overhead.

### Benchmark 4: Latency Percentiles

```cpp
for (size_t i = 0; i < N; ++i) {
    // ... generate order ...
    auto a = Clock::now();
    book.add_order(o);
    auto b = Clock::now();
    lat[i] = duration_cast<nanoseconds>(b - a).count();
}
std::sort(lat.begin(), lat.end());
```

Wraps each `add_order` call with `steady_clock::now()` to measure
per-order latency. After collecting N samples, sorts them and reports
percentiles (p50, p90, p99, p99.9, max).

### Benchmark 5: Multi-Thread Aggregate

```cpp
static WorkerResult worker_matching(int core, size_t N, uint64_t seed) {
    pin_to_core(core);
    OrderArena arena(N + 2);            // each thread has its own arena
    FlatOrderBook book(Symbol(core), MIN_P, MAX_P, arena.base());  // own book
    SplitMix64 rng(seed);              // own RNG with unique seed
    // ... process N orders ...
}
```

Each thread creates its own `OrderArena` + `FlatOrderBook` — **zero shared
state** between threads. No locks, no atomics, no cache-line bouncing.
This is the same architecture used by real exchanges: one matching thread
per symbol.

The `run_mt()` function spawns `num_threads` workers, waits for all to
complete, then reports:
- **Wall time:** real clock time from first launch to last join
- **Aggregate throughput:** total orders across all threads / wall time
- **Per-thread throughput:** each worker's rate

### Benchmark 6: Scaling Curve + Projection

```cpp
for (unsigned nt = 1; nt <= hw; ++nt) {
    // launch nt threads, measure wall time
    // report aggregate throughput
}

// Extrapolate to larger core counts
for (unsigned c : {8, 16, 32, 64, 128}) {
    double proj = single_rate * c;
    // "64 cores → 4.78 B orders/sec"
}
```

Runs the benchmark with 1, 2, 3, ... N threads (N = hardware thread count)
and reports the scaling curve. Then projects to higher core counts by
multiplying the single-core rate (since scaling is linear with zero
shared state).

---

## 9. `Makefile` — Build System

```makefile
CXX       := g++
CXXFLAGS  := -std=c++20 -Wall -Wextra -Wpedantic -Iinclude
RELEASE   := -O3 -march=native -DNDEBUG -flto -funroll-loops
```

| Flag | Purpose |
|---|---|
| `-std=c++20` | C++20 for `[[likely]]`, `[[unlikely]]`, `[[nodiscard]]` |
| `-O3` | Maximum optimisation (auto-vectorisation, aggressive inlining) |
| `-march=native` | Generate instructions for this specific CPU (AVX2, BMI2, etc.) |
| `-DNDEBUG` | Disable `assert()` and standard library debug checks |
| `-flto` | Link-Time Optimisation: cross-file inlining, dead code elimination |
| `-funroll-loops` | Unroll short loops to reduce branch overhead |

**PGO (Profile-Guided Optimisation):**
```makefile
pgo:
    g++ ... -fprofile-generate ... -o ome_bench    # Step 1: instrumented build
    ./ome_bench                                     # Step 2: run to collect profiles
    g++ ... -fprofile-use -fprofile-correction ...  # Step 3: optimised rebuild
```

PGO collects real branch-taken statistics during the profiling run, then
the compiler uses this data to:
- Place hot code paths sequentially (fewer instruction cache misses)
- Optimise branch prediction for real workload patterns
- Size inline decisions based on actual call frequencies

---

## 10. How It All Fits Together — Request Flow

Here is the complete path of a single order through the engine:

```
User calls: engine.submit_order(AAPL, Buy, Limit, 15025, 80)
    │
    ├─ 1. MatchingEngine::submit_order()         [matching_engine.hpp]
    │      books_.find(AAPL)                      → O(1) hash lookup
    │      arena_.allocate(AAPL, Buy, Limit, ...)
    │      │
    │      ├─ 2. OrderArena::allocate()           [types.hpp]
    │      │      storage_[next_++].init(...)      → O(1) bump allocator
    │      │      return &storage_[idx]
    │      │
    │      └─ 3. FlatOrderBook::add_order(order)  [order_book.hpp]
    │             │
    │             ├─ 4. Dispatch: order is Limit+Buy
    │             │      → match_limit_buy(order, idx, result)
    │             │
    │             ├─ 5. match_limit_buy:
    │             │      Check: order.price(15025) >= best_ask_(15025)?  YES
    │             │      │
    │             │      ├─ level_at(15025)         → O(1) array index
    │             │      │  head order: SELL 50 @ 150.25
    │             │      │  fill = min(80, 50) = 50
    │             │      │
    │             │      ├─ exec_fill(buy, sell, 50)
    │             │      │  buy.filled_quantity  += 50  → now 50
    │             │      │  sell.filled_quantity += 50  → now 50 (FILLED)
    │             │      │  trade_count_++, total_volume_ += 50
    │             │      │
    │             │      ├─ sell is filled → pop_front(), --resting_count_
    │             │      │  level is now empty → advance_best_ask()
    │             │      │  scan: 15026 empty, 15050 has orders → best_ask_ = 15050
    │             │      │
    │             │      └─ Check: order.price(15025) >= best_ask_(15050)?  NO
    │             │         Exit matching loop. order.remaining() = 30.
    │             │
    │             └─ 6. Rest unfilled portion:
    │                    order is not filled, is Limit, not Cancelled
    │                    level_at(15025).push_back(order)  → O(1) DLL append
    │                    resting_count_++
    │                    15025 > best_bid_(15000) → best_bid_ = 15025
    │
    └─ 7. Back in MatchingEngine::submit_order():
           stats_.trades_executed += 1
           stats_.total_volume += 50
           order is not fully filled, has partial → stats_.orders_partial++
           return order.id (the arena index)
```

**Total operations for this order:**
- 1 hash lookup (symbol routing)
- 1 arena allocation (counter increment + 12 field writes)
- 1 array access (level lookup for matching)
- 1 price comparison (match check)
- 6 integer increments (trade execution)
- 1 linked-list pop (remove filled resting order)
- ~3 array accesses (advance best_ask scan)
- 1 array access (level lookup for insertion)
- 1 linked-list append (rest the order)
- 1 scalar comparison (update best_bid)

**~15-20 memory operations total, completing in ~38 ns.**
