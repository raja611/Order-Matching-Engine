#include "order_book.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

using namespace ome;
using Clock = std::chrono::steady_clock;

static void pin_to_core([[maybe_unused]] int core) {
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#endif
}

static void hdr(const char* title) {
    std::cout << "\n" << std::string(62, '=') << "\n " << title
              << "\n" << std::string(62, '=') << "\n";
}

struct GenOrder {
    Side     side;
    Price    price;
    Quantity qty;
};

static void bench_raw(size_t N) {
    pin_to_core(0);
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "SINGLE-THREAD  MATCHING  (%zuM orders)", N / 1'000'000);
    hdr(buf);

    constexpr Price MIN_P = 9000, MAX_P = 11000;
    OrderArena arena(N + 2);
    FlatOrderBook book(1, MIN_P, MAX_P, arena.base());
    SplitMix64 rng(42);

    auto t0 = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        uint64_t r = rng.next();
        Side  s = (r & 1) ? Side::Buy : Side::Sell;
        Price p = 9900 + static_cast<int32_t>((r >> 1) % 201);
        Quantity q = 1 + static_cast<Quantity>((r >> 9) % 100);
        Order* o = arena.allocate_fast(
            static_cast<OrderId>(arena.used()), s, p, q);
        book.add_order(o);
    }
    auto t1 = Clock::now();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << std::fixed << std::setprecision(2)
              << "  Time        : " << sec * 1e3 << " ms\n"
              << "  Throughput  : " << (N / sec) / 1e6 << " M orders/sec\n"
              << std::setprecision(1)
              << "  Avg latency : " << (sec * 1e9 / N) << " ns/order\n"
              << "  Trades      : " << book.trade_count() << "\n";
}

static void bench_insert_pipelined(const char* tag, size_t N,
                                    Price buy_lo, Price buy_hi,
                                    Price sell_lo, Price sell_hi) {
    pin_to_core(0);
    Price min_p = std::min(buy_lo, sell_lo);
    Price max_p = std::max(buy_hi, sell_hi);

    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "SINGLE-THREAD  INSERT-ONLY %s  (%zuM, prefetched)", tag, N / 1'000'000);
    hdr(buf);

    OrderArena arena(N + 2);
    FlatOrderBook book(1, min_p, max_p, arena.base());
    SplitMix64 rng(77);

    uint32_t br = static_cast<uint32_t>(buy_hi  - buy_lo  + 1);
    uint32_t sr = static_cast<uint32_t>(sell_hi - sell_lo + 1);

    auto gen = [&](uint64_t r) -> GenOrder {
        bool buy = r & 1;
        Price p = buy ? (buy_lo  + static_cast<int32_t>((r >> 1) % br))
                      : (sell_lo + static_cast<int32_t>((r >> 1) % sr));
        Quantity q = 1 + static_cast<Quantity>((r >> 17) % 100);
        return { buy ? Side::Buy : Side::Sell, p, q };
    };

    uint64_t r0 = rng.next();
    auto g0 = gen(r0);
    Order* cur = arena.allocate_fast(
        static_cast<OrderId>(arena.used()), g0.side, g0.price, g0.qty);

    auto t0 = Clock::now();
    for (size_t i = 1; i < N; ++i) {
        uint64_t r = rng.next();
        auto g = gen(r);
        Order* nxt = arena.allocate_fast(
            static_cast<OrderId>(arena.used()), g.side, g.price, g.qty);
        book.prefetch_for_insert(g.price);
        book.add_order(cur);
        cur = nxt;
    }
    book.add_order(cur);
    auto t1 = Clock::now();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << std::fixed << std::setprecision(2)
              << "  Time        : " << sec * 1e3 << " ms\n"
              << "  Throughput  : " << (N / sec) / 1e6 << " M orders/sec\n"
              << std::setprecision(1)
              << "  Avg latency : " << (sec * 1e9 / N) << " ns/order\n"
              << "  Levels      : " << (max_p - min_p + 1)
              << " (" << (max_p - min_p + 1) * 16 / 1024 << " KB)\n";
}

static void bench_insert(const char* tag, size_t N,
                          Price buy_lo, Price buy_hi,
                          Price sell_lo, Price sell_hi) {
    pin_to_core(0);
    Price min_p = std::min(buy_lo, sell_lo);
    Price max_p = std::max(buy_hi, sell_hi);

    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "SINGLE-THREAD  INSERT-ONLY %s  (%zuM)", tag, N / 1'000'000);
    hdr(buf);

    OrderArena arena(N + 2);
    FlatOrderBook book(1, min_p, max_p, arena.base());
    SplitMix64 rng(77);
    uint32_t br = static_cast<uint32_t>(buy_hi  - buy_lo  + 1);
    uint32_t sr = static_cast<uint32_t>(sell_hi - sell_lo + 1);

    auto t0 = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        uint64_t r = rng.next();
        bool buy = r & 1;
        Price p = buy ? (buy_lo  + static_cast<int32_t>((r >> 1) % br))
                      : (sell_lo + static_cast<int32_t>((r >> 1) % sr));
        Quantity q = 1 + static_cast<Quantity>((r >> 17) % 100);
        Order* o = arena.allocate_fast(
            static_cast<OrderId>(arena.used()),
            buy ? Side::Buy : Side::Sell, p, q);
        book.add_order(o);
    }
    auto t1 = Clock::now();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << std::fixed << std::setprecision(2)
              << "  Time        : " << sec * 1e3 << " ms\n"
              << "  Throughput  : " << (N / sec) / 1e6 << " M orders/sec\n"
              << std::setprecision(1)
              << "  Avg latency : " << (sec * 1e9 / N) << " ns/order\n"
              << "  Levels      : " << (max_p - min_p + 1)
              << " (" << (max_p - min_p + 1) * 16 / 1024 << " KB)\n";
}

static void bench_latency(size_t N) {
    pin_to_core(0);
    hdr("SINGLE-THREAD  LATENCY");

    constexpr Price MIN_P = 9000, MAX_P = 11000;
    OrderArena arena(N + 2);
    FlatOrderBook book(1, MIN_P, MAX_P, arena.base());
    SplitMix64 rng(123);

    std::vector<uint64_t> lat(N);
    for (size_t i = 0; i < N; ++i) {
        uint64_t r = rng.next();
        Side  s = (r & 1) ? Side::Buy : Side::Sell;
        Price p = 9900 + static_cast<int32_t>((r >> 1) % 201);
        Quantity q = 1 + static_cast<Quantity>((r >> 9) % 100);
        Order* o = arena.allocate_fast(
            static_cast<OrderId>(arena.used()), s, p, q);

        auto a = Clock::now();
        book.add_order(o);
        auto b = Clock::now();
        lat[i] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }

    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) {
        return lat[static_cast<size_t>(p / 100.0 * (lat.size() - 1))];
    };
    double mean = std::accumulate(lat.begin(), lat.end(), 0.0) / lat.size();

    std::cout << std::fixed << std::setprecision(0)
              << "  Min    : " << lat.front()  << " ns\n"
              << "  Mean   : " << mean          << " ns\n"
              << "  p50    : " << pct(50)       << " ns\n"
              << "  p90    : " << pct(90)       << " ns\n"
              << "  p99    : " << pct(99)       << " ns\n"
              << "  p99.9  : " << pct(99.9)     << " ns\n"
              << "  Max    : " << lat.back()    << " ns\n";
}

static void bench_cancel(size_t N) {
    pin_to_core(0);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "SINGLE-THREAD  CANCEL  (%zuK)", N / 1'000);
    hdr(buf);

    constexpr Price MIN_P = 5000, MAX_P = 15000;
    OrderArena arena(N + 2);
    FlatOrderBook book(1, MIN_P, MAX_P, arena.base());
    SplitMix64 rng(99);

    std::vector<Order*> orders;
    orders.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        uint64_t r = rng.next();
        bool buy = r & 1;
        Price p = buy ? (5000 + static_cast<int32_t>((r >> 1) % 4001))
                      : (11000 + static_cast<int32_t>((r >> 1) % 4001));
        Quantity q = 1 + static_cast<Quantity>((r >> 14) % 100);
        Order* o = arena.allocate_fast(
            static_cast<OrderId>(arena.used()),
            buy ? Side::Buy : Side::Sell, p, q);
        book.add_order(o);
        orders.push_back(o);
    }

    SplitMix64 shuf(42);
    for (size_t i = N - 1; i > 0; --i)
        std::swap(orders[i], orders[shuf.next_u32() % (i + 1)]);

    auto t0 = Clock::now();
    for (auto* o : orders) book.cancel_order(o);
    auto t1 = Clock::now();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << std::fixed << std::setprecision(1)
              << "  Avg cancel : " << (sec * 1e9 / N) << " ns\n"
              << std::setprecision(2)
              << "  Throughput : " << (N / sec) / 1e6 << " M cancels/sec\n";
}

struct WorkerResult {
    uint64_t orders  = 0;
    uint64_t trades  = 0;
    double   elapsed = 0;
};

static WorkerResult worker_matching(int core, size_t N, uint64_t seed) {
    pin_to_core(core);
    constexpr Price MIN_P = 9000, MAX_P = 11000;
    OrderArena arena(N + 2);
    FlatOrderBook book(static_cast<Symbol>(core), MIN_P, MAX_P, arena.base());
    SplitMix64 rng(seed);

    auto t0 = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        uint64_t r = rng.next();
        Side  s = (r & 1) ? Side::Buy : Side::Sell;
        Price p = 9900 + static_cast<int32_t>((r >> 1) % 201);
        Quantity q = 1 + static_cast<Quantity>((r >> 9) % 100);
        Order* o = arena.allocate_fast(
            static_cast<OrderId>(arena.used()), s, p, q);
        book.add_order(o);
    }
    auto t1 = Clock::now();

    return { N, book.trade_count(),
             std::chrono::duration<double>(t1 - t0).count() };
}

static WorkerResult worker_insert_pipelined(int core, size_t N, uint64_t seed,
                                             Price blo, Price bhi,
                                             Price slo, Price shi) {
    pin_to_core(core);
    Price min_p = std::min(blo, slo), max_p = std::max(bhi, shi);
    uint32_t br = static_cast<uint32_t>(bhi - blo + 1);
    uint32_t sr = static_cast<uint32_t>(shi - slo + 1);

    OrderArena arena(N + 2);
    FlatOrderBook book(static_cast<Symbol>(core), min_p, max_p, arena.base());
    SplitMix64 rng(seed);

    auto gen = [&](uint64_t r) -> GenOrder {
        bool buy = r & 1;
        Price p = buy ? (blo + static_cast<int32_t>((r >> 1) % br))
                      : (slo + static_cast<int32_t>((r >> 1) % sr));
        Quantity q = 1 + static_cast<Quantity>((r >> 17) % 100);
        return { buy ? Side::Buy : Side::Sell, p, q };
    };

    auto g0 = gen(rng.next());
    Order* cur = arena.allocate_fast(
        static_cast<OrderId>(arena.used()), g0.side, g0.price, g0.qty);

    auto t0 = Clock::now();
    for (size_t i = 1; i < N; ++i) {
        auto g = gen(rng.next());
        Order* nxt = arena.allocate_fast(
            static_cast<OrderId>(arena.used()), g.side, g.price, g.qty);
        book.prefetch_for_insert(g.price);
        book.add_order(cur);
        cur = nxt;
    }
    book.add_order(cur);
    auto t1 = Clock::now();

    return { N, book.trade_count(),
             std::chrono::duration<double>(t1 - t0).count() };
}

static void run_mt(const char* label, unsigned nt, size_t per,
                    bool pipelined, Price blo, Price bhi, Price slo, Price shi) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "MULTI-THREAD  %s  (%u x %zuM%s)", label, nt, per / 1'000'000,
        pipelined ? " prefetched" : "");
    hdr(buf);

    std::vector<WorkerResult> res(nt);
    std::vector<std::thread> threads;
    auto w0 = Clock::now();
    for (unsigned t = 0; t < nt; ++t)
        threads.emplace_back([&, t]() {
            if (pipelined)
                res[t] = worker_insert_pipelined(
                    static_cast<int>(t), per, 42 + t * 7919, blo, bhi, slo, shi);
            else
                res[t] = worker_matching(static_cast<int>(t), per, 42 + t * 7919);
        });
    for (auto& th : threads) th.join();
    auto w1 = Clock::now();

    double wall = std::chrono::duration<double>(w1 - w0).count();
    uint64_t tot = 0, trades = 0;
    for (auto& r : res) { tot += r.orders; trades += r.trades; }

    double agg = tot / wall;
    std::cout << std::fixed << std::setprecision(2)
              << "  Wall          : " << wall * 1e3 << " ms\n"
              << "  Trades        : " << trades << "\n"
              << "  AGGREGATE     : " << agg / 1e6 << " M orders/sec";
    if (agg >= 1e9)
        std::cout << "  ★ " << agg / 1e9 << " BILLION/sec ★";
    std::cout << "\n  Per-thread:\n";
    for (unsigned t = 0; t < nt; ++t)
        std::cout << "    [" << t << "] " << std::setprecision(1)
                  << (res[t].orders / res[t].elapsed) / 1e6 << " M/s\n";
}

static void scaling(size_t per, Price blo, Price bhi, Price slo, Price shi) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    hdr("SCALING + PROJECTION  (insert-only, prefetched)");
    std::cout << std::setw(6) << "Thr" << " | "
              << std::setw(14) << "Agg (M/s)" << " | "
              << std::setw(14) << "Per-core(M/s)" << "\n"
              << std::string(42, '-') << "\n";

    double single_rate = 0;

    for (unsigned nt = 1; nt <= hw; ++nt) {
        std::vector<WorkerResult> res(nt);
        std::vector<std::thread> threads;
        auto t0 = Clock::now();
        for (unsigned t = 0; t < nt; ++t)
            threads.emplace_back([&, t]() {
                res[t] = worker_insert_pipelined(
                    static_cast<int>(t), per, 42 + t * 7919, blo, bhi, slo, shi);
            });
        for (auto& th : threads) th.join();
        auto t1 = Clock::now();

        double wall = std::chrono::duration<double>(t1 - t0).count();
        double agg = (static_cast<uint64_t>(nt) * per) / wall;
        double pc  = per / res[0].elapsed;
        if (nt == 1) single_rate = pc;

        std::cout << std::fixed << std::setprecision(1)
                  << std::setw(6) << nt << " | "
                  << std::setw(14) << agg / 1e6 << " | "
                  << std::setw(14) << pc / 1e6 << "\n";
    }

    std::cout << "\n  Per-core rate: "
              << std::fixed << std::setprecision(1)
              << single_rate / 1e6 << " M orders/sec ("
              << std::setprecision(1) << 1e9 / single_rate << " ns/order)\n"
              << "\n  Projected aggregate (linear scaling):\n";
    for (unsigned c : {8u, 16u, 32u, 64u, 128u}) {
        double proj = single_rate * c;
        std::cout << "    " << std::setw(4) << c << " cores -> "
                  << std::setprecision(2) << proj / 1e9 << " B orders/sec";
        if (proj >= 1e9) std::cout << " ★";
        std::cout << "\n";
    }
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════╗\n"
              << "║  Order Matching Engine — Ultra Performance Benchmark ║\n"
              << "╚══════════════════════════════════════════════════════╝\n";

    unsigned hw = std::thread::hardware_concurrency();
    std::cout << "\nHardware threads: " << hw
              << "  |  sizeof(Order)=" << sizeof(Order)
              << "  sizeof(PriceLevel)=" << sizeof(PriceLevel) << "\n";

    bench_raw(1'000'000);
    bench_raw(10'000'000);

    bench_insert("(narrow)", 10'000'000, 9000, 9500, 10500, 11000);
    bench_insert_pipelined("(narrow)", 10'000'000, 9000, 9500, 10500, 11000);

    bench_latency(1'000'000);
    bench_cancel(500'000);

    run_mt("MATCHING", hw, 10'000'000, false, 0, 0, 0, 0);
    run_mt("INSERT-ONLY", hw, 10'000'000, true, 9000, 9500, 10500, 11000);

    scaling(5'000'000, 9000, 9500, 10500, 11000);

    std::cout << "\nDone.\n";
    return 0;
}
