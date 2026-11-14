#include "../src/opponent_history.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using vvw::history::HistoryCell;
using vvw::history::OpponentHistory;
using vvw::history::Species;

static std::vector<HistoryCell> makeBoard(std::size_t width,
                                          std::size_t height,
                                          int enemy_x,
                                          int enemy_y) {
    std::vector<HistoryCell> cells(width * height);
    const std::size_t house_index = 2 * width + 6;
    cells[house_index] = HistoryCell(12, 0, 0, true);
    cells[static_cast<std::size_t>(enemy_y) * width + static_cast<std::size_t>(enemy_x)] =
        HistoryCell(0, 0, 8, false);
    return cells;
}

int main() {
    const std::size_t width = 8;
    const std::size_t height = 5;
    OpponentHistory history;

    // Werewolf group approaches the house: x=2 -> 3 -> 4.
    history.record(width, height, makeBoard(width, height, 2, 2), 1);
    history.record(width, height, makeBoard(width, height, 3, 2), 2);
    history.record(width, height, makeBoard(width, height, 4, 2), 3);

    assert(history.size() == 3);
    const std::vector<vvw::history::HouseTrend> trends =
        history.houseApproachTrends(Species::Werewolves);
    assert(trends.size() == 1);
    assert(trends[0].oldest_distance == 4);
    assert(trends[0].newest_distance == 2);
    assert(std::fabs(trends[0].average_approach_per_turn - 1.0) < 1e-9);
    assert(std::fabs(trends[0].confidence - 1.0) < 1e-9);
    assert(std::fabs(trends[0].risk - 1.0) < 1e-9);

    const double discounted = OpponentHistory::applyHouseHistoryDiscount(
        100.0, trends[0].risk, true);
    assert(std::fabs(discounted - 90.0) < 1e-9);

    // Not contested => no history discount.
    const double unchanged = OpponentHistory::applyHouseHistoryDiscount(
        100.0, trends[0].risk, false);
    assert(std::fabs(unchanged - 100.0) < 1e-9);

    // Ring buffer stays bounded and overwrites its oldest snapshot.
    history.record(width, height, makeBoard(width, height, 5, 2), 4);
    history.record(width, height, makeBoard(width, height, 5, 3), 5);
    assert(history.size() == OpponentHistory::kCapacity);
    assert(history.newest(0).update_number == 5);
    assert(history.newest(3).update_number == 2);

    std::cout << "OpponentHistory tests passed\n";
    return 0;
}
