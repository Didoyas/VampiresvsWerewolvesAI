
#include "../src/opponent_history.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace vvw::ngs;

namespace {

std::vector<HistoryCell> makeHistoryBoard(
    std::size_t width,
    std::size_t height,
    int enemyX,
    int enemyY
) {
    std::vector<HistoryCell> cells(width * height);

    const std::size_t houseIndex = 2 * width + 6;

    cells[houseIndex] = HistoryCell(
        12, 0, 0, true
    );

    const std::size_t enemyIndex =
        static_cast<std::size_t>(enemyY) * width +
        static_cast<std::size_t>(enemyX);

    cells[enemyIndex] = HistoryCell(
        0, 0, 8, false
    );

    return cells;
}

BoardState makeBoard() {
    BoardState board;

    board.rows = 4;
    board.columns = 4;
    board.cells.resize(16);

    board.ourSpecies = Species::Vampires;

    board.at({1, 1}).werewolves = 2;
    board.at({3, 2}).werewolves = 3;

    board.at({0, 0}).vampires = 5;

    return board;
}

// -------------------------------------------------------------
// Original implementation tests
// -------------------------------------------------------------

void testOriginalHouseApproachTrend() {
    const std::size_t width = 8;
    const std::size_t height = 5;

    OpponentHistory history(Species::Werewolves);

    // Enemy group approaches the house:
    // x = 2 -> 3 -> 4.
    history.record(
        width, height,
        makeHistoryBoard(width, height, 2, 2),
        1
    );

    history.record(
        width, height,
        makeHistoryBoard(width, height, 3, 2),
        2
    );

    history.record(
        width, height,
        makeHistoryBoard(width, height, 4, 2),
        3
    );

    assert(history.size() == 3);
    assert(!history.empty());

    const std::vector<HouseTrend> trends =
        history.houseApproachTrends(Species::Werewolves);

    assert(trends.size() == 1);

    assert(trends[0].oldest_distance == 4);
    assert(trends[0].newest_distance == 2);

    assert(
        std::abs(
            trends[0].average_approach_per_turn - 1.0
        ) < 1e-9
    );

    assert(
        std::abs(trends[0].confidence - 1.0) < 1e-9
    );

    assert(
        std::abs(trends[0].risk - 1.0) < 1e-9
    );
}

void testOriginalHouseDiscount() {
    const double discounted =
        OpponentHistory::applyHouseHistoryDiscount(
            100.0,
            1.0,
            true
        );

    assert(std::abs(discounted - 90.0) < 1e-9);

    const double unchanged =
        OpponentHistory::applyHouseHistoryDiscount(
            100.0,
            1.0,
            false
        );

    assert(std::abs(unchanged - 100.0) < 1e-9);

    const double partialDiscount =
        OpponentHistory::applyHouseHistoryDiscount(
            100.0,
            0.5,
            true
        );

    assert(std::abs(partialDiscount - 95.0) < 1e-9);
}

void testOriginalNewestAccess() {
    OpponentHistory history(Species::Werewolves);

    const std::size_t width = 8;
    const std::size_t height = 5;

    history.record(
        width, height,
        makeHistoryBoard(width, height, 2, 2),
        1
    );

    history.record(
        width, height,
        makeHistoryBoard(width, height, 3, 2),
        2
    );

    assert(history.newest(0).update_number == 2);
    assert(history.newest(1).update_number == 1);

    assert(history.newest(0).turn == 2);
}

void testOriginalInsufficientSnapshots() {
    OpponentHistory history(Species::Werewolves);

    history.record(
        8, 5,
        makeHistoryBoard(8, 5, 2, 2),
        1
    );

    const std::vector<HouseTrend> trends =
        history.houseApproachTrends(Species::Werewolves);

    assert(trends.empty());
}

void testOriginalNoEnemyMakesTrendAmbiguous() {
    OpponentHistory history(Species::Werewolves);

    std::vector<HistoryCell> first =
        makeHistoryBoard(8, 5, 2, 2);

    std::vector<HistoryCell> second =
        makeHistoryBoard(8, 5, 2, 2);

    // Remove all enemies from the second observation.
    for (HistoryCell& cell : second) {
        cell.werewolves = 0;
    }

    history.record(8, 5, first, 1);
    history.record(8, 5, second, 2);

    const std::vector<HouseTrend> trends =
        history.houseApproachTrends(Species::Werewolves);

    assert(trends.empty());
}

// -------------------------------------------------------------
// Snapshot management tests
// -------------------------------------------------------------

void testConfigurableCapacity() {
    OpponentHistory history(
        Species::Werewolves,
        2
    );

    assert(history.isConfigured());
    assert(history.capacity() == 2);

    history.record(
        8, 5,
        makeHistoryBoard(8, 5, 2, 2),
        1
    );

    history.record(
        8, 5,
        makeHistoryBoard(8, 5, 3, 2),
        2
    );

    history.record(
        8, 5,
        makeHistoryBoard(8, 5, 4, 2),
        3
    );

    assert(history.size() == 2);

    assert(history.newest(0).update_number == 3);
    assert(history.newest(1).update_number == 2);
}


void testRejectConfirmedMovementForUnknownSpecies() {
    OpponentHistory history;

    const OpponentHistoryError result =
        history.recordConfirmedMovement(
            1,
            {1, 1},
            {2, 1},
            2
        );

    assert(result == OpponentHistoryError::InvalidSpecies);
}

void testRejectsDuplicateSnapshot() {
    OpponentHistory history(Species::Werewolves);

    const auto cells =
        makeHistoryBoard(8, 5, 2, 2);

    history.record(8, 5, cells, 1);

    bool threw = false;

    try {
        history.record(8, 5, cells, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
    assert(history.size() == 1);
}

void testRejectsChangedDimensions() {
    OpponentHistory history(Species::Werewolves);

    history.record(
        8, 5,
        makeHistoryBoard(8, 5, 2, 2),
        1
    );

    bool threw = false;

    try {
        history.record(
            7, 5,
            makeHistoryBoard(7, 5, 2, 2),
            2
        );
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
    assert(history.size() == 1);
}

void testClearHistory() {
    OpponentHistory history(Species::Werewolves);

    history.record(
        8, 5,
        makeHistoryBoard(8, 5, 2, 2),
        1
    );

    history.clear();

    assert(history.empty());
    assert(history.size() == 0);
    assert(history.confirmedMovements().empty());
}

// -------------------------------------------------------------
// BoardState integration tests
// -------------------------------------------------------------

void testRecordObservationFromBoardState() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Werewolves);

    const HistoryUpdateResult result =
        history.recordObservation(board, 1);

    assert(result.valid());
    assert(result.positionsObserved == 2);
    assert(result.snapshotsRetained == 1);

    assert(history.size() == 1);

    const BoardSnapshot& snapshot = history.newest();

    assert(snapshot.width == 4);
    assert(snapshot.height == 4);
    assert(snapshot.turn == 1);

    assert(snapshot.cells.size() == 16);
    assert(snapshot.cells[5].werewolves == 2);
    assert(snapshot.cells[11].werewolves == 3);
}

void testRecordObservationRejectsWrongSpecies() {
    BoardState board = makeBoard();

    // Vampires are our species, so the opponent is Werewolves.
    OpponentHistory history(Species::Vampires);

    const HistoryUpdateResult result =
        history.recordObservation(board, 1);

    assert(!result.valid());
    assert(result.error == OpponentHistoryError::InvalidSpecies);
}

void testRecordObservationRejectsInvalidTurn() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Werewolves);

    assert(history.recordObservation(board, 5).valid());

    const HistoryUpdateResult result =
        history.recordObservation(board, 5);

    assert(!result.valid());
    assert(result.error == OpponentHistoryError::InvalidTurn);
}

// -------------------------------------------------------------
// Position statistics tests
// -------------------------------------------------------------

void testPositionStatistics() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    board.at({1, 1}).werewolves = 3;
    assert(history.recordObservation(board, 2).valid());

    board.at({1, 1}).werewolves = 0;
    board.at({2, 2}).werewolves = 4;

    assert(history.recordObservation(board, 3).valid());

    const PositionStatisticsResult result =
        history.getPositionStatistics(
            {1, 1},
            3,
            1.0
        );

    assert(result.valid());

    const OpponentPositionStatistics& stats =
        result.statistics;

    assert(stats.snapshotsConsidered == 3);
    assert(stats.presenceCount == 2);

    assert(
        std::abs(
            stats.presenceFrequency - (2.0 / 3.0)
        ) < 1e-9
    );

    const double expectedWeightedFrequency =
        0.75 / 1.75;

    assert(
        std::abs(
            stats.recencyWeightedPresenceFrequency -
            expectedWeightedFrequency
        ) < 1e-9
    );

    assert(stats.latestPopulation == 0);
    assert(stats.maximumObservedPopulation == 3);

    assert(stats.hasBeenObserved);
    assert(stats.lastSeenTurn == 2);
}

void testPositionNeverObserved() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    const PositionStatisticsResult result =
        history.getPositionStatistics(
            {2, 2},
            1
        );

    assert(result.valid());

    assert(result.statistics.presenceCount == 0);
    assert(result.statistics.presenceFrequency == 0.0);
    assert(!result.statistics.hasBeenObserved);
}

void testPositionStatisticsRejectInvalidHalfLife() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    const PositionStatisticsResult result =
        history.getPositionStatistics(
            {1, 1},
            1,
            0.0
        );

    assert(!result.valid());
    assert(result.error == OpponentHistoryError::InvalidHalfLife);
}

// -------------------------------------------------------------
// Confirmed movement tests
// -------------------------------------------------------------

void testConfirmedMovement() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    const OpponentHistoryError error =
        history.recordConfirmedMovement(
            1,
            {1, 1},
            {2, 1},
            2
        );

    assert(error == OpponentHistoryError::None);
    assert(history.confirmedMovements().size() == 1);

    const OpponentMovementObservation& movement =
        history.confirmedMovements().front();

    assert(movement.turn == 1);
    assert((movement.source == Position{1, 1}));
    assert((movement.destination == Position{2, 1}));
    assert(movement.count == 2);
}

void testRejectInvalidMovement() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {4, 1}, 2
        ) == OpponentHistoryError::InvalidPosition
    );

    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {3, 3}, 2
        ) == OpponentHistoryError::InvalidMovement
    );

    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {1, 1}, 2
        ) == OpponentHistoryError::InvalidMovement
    );

    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {2, 1}, 0
        ) == OpponentHistoryError::InvalidMovement
    );

    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {2, 1}, 256
        ) == OpponentHistoryError::InvalidMovement
    );

    assert(history.confirmedMovements().empty());
}

void testSnapshotDoesNotInferMovement() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    board.at({1, 1}).werewolves = 0;
    board.at({2, 1}).werewolves = 2;

    assert(history.recordObservation(board, 2).valid());

    // Snapshot changes alone do not establish a confirmed move.
    assert(history.confirmedMovements().empty());
}

void testOriginalDefaultConstructor() {
    OpponentHistory history;

    assert(history.empty());

    history.record(
        8,
        5,
        makeHistoryBoard(8, 5, 2, 2),
        1
    );

    history.record(
        8,
        5,
        makeHistoryBoard(8, 5, 3, 2),
        2
    );

    assert(history.size() == 2);

    const auto trends =
        history.houseApproachTrends(Species::Werewolves);

    assert(trends.size() == 1);
    assert(trends[0].oldest_distance == 4);
    assert(trends[0].newest_distance == 3);
}

} // namespace

int main() {
    // Original implementation tests.
    testOriginalDefaultConstructor();
    testOriginalHouseApproachTrend();
    testOriginalHouseDiscount();
    testOriginalNewestAccess();
    testOriginalInsufficientSnapshots();
    testOriginalNoEnemyMakesTrendAmbiguous();
    testRejectConfirmedMovementForUnknownSpecies();

    // Snapshot management tests.
    testConfigurableCapacity();
    testRejectsDuplicateSnapshot();
    testRejectsChangedDimensions();
    testClearHistory();

    // NGS BoardState integration.
    testRecordObservationFromBoardState();
    testRecordObservationRejectsWrongSpecies();
    testRecordObservationRejectsInvalidTurn();

    // New position statistics.
    testPositionStatistics();
    testPositionNeverObserved();
    testPositionStatisticsRejectInvalidHalfLife();

    // New confirmed movement history.
    testConfirmedMovement();
    testRejectInvalidMovement();
    testSnapshotDoesNotInferMovement();

    std::cout << "All consolidated opponent history tests passed.\n";

    return 0;
}
