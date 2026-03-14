#include "matching_engine.hpp"
#include <iostream>

using namespace ome;

int main() {
    std::cout << "╔══════════════════════════════════════════════╗\n"
              << "║   Low-Latency Order Matching Engine (C++)    ║\n"
              << "╚══════════════════════════════════════════════╝\n\n";

    MatchingEngine engine(100'000);
    constexpr Symbol AAPL = 1;
    engine.add_symbol(AAPL, 14000, 16000);  // price range $140.00–$160.00

    std::cout << ">>> Placing resting sell orders (asks)...\n";
    engine.submit_order(AAPL, Side::Sell, OrderType::Limit, 15050, 100);
    engine.submit_order(AAPL, Side::Sell, OrderType::Limit, 15075, 200);
    engine.submit_order(AAPL, Side::Sell, OrderType::Limit, 15100, 300);
    engine.submit_order(AAPL, Side::Sell, OrderType::Limit, 15025, 50);

    std::cout << ">>> Placing resting buy orders (bids)...\n";
    engine.submit_order(AAPL, Side::Buy, OrderType::Limit, 14975, 150);
    engine.submit_order(AAPL, Side::Buy, OrderType::Limit, 14950, 250);
    engine.submit_order(AAPL, Side::Buy, OrderType::Limit, 15000, 100);
    engine.submit_order(AAPL, Side::Buy, OrderType::Limit, 14900, 500);
    engine.print_book(AAPL);

    std::cout << ">>> Aggressive BUY 80 @ 150.25 (crosses spread)...\n";
    engine.submit_order(AAPL, Side::Buy, OrderType::Limit, 15025, 80);
    engine.print_book(AAPL);

    std::cout << ">>> Market BUY 200 (sweeps ask side)...\n";
    engine.submit_order(AAPL, Side::Buy, OrderType::Market, 0, 200);
    engine.print_book(AAPL);

    std::cout << ">>> Placing sell #10, then cancelling it...\n";
    OrderId oid = engine.submit_order(AAPL, Side::Sell, OrderType::Limit, 15200, 400);
    std::cout << "    Placed order #" << oid << "\n";
    engine.print_book(AAPL);
    bool cancelled = engine.cancel_order(AAPL, oid);
    std::cout << "    Cancel #" << oid << ": " << (cancelled ? "OK" : "FAILED") << "\n";
    engine.print_book(AAPL);

    std::cout << ">>> Sell 500 @ 149.00 — sweeps bid levels...\n";
    engine.submit_order(AAPL, Side::Sell, OrderType::Limit, 14900, 500);
    engine.print_book(AAPL);

    engine.print_stats();
    return 0;
}
