
#include "../src/opponent_history.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using namespace vvw::ngs;

namespace {

BoardState makeBoard() {
    BoardState board;

    board.rows = 4;
    board.columns = 4;
    board.cells.resize(16);

    board.ourSpecies = Species::Vampires;

    // Opponent populations.
    board.at({1, 1}).werewolves = 2;
    board.at({3, 2}).werewolves = 3;

    // Our own population, which must not be recorded
    // as an opponent position.
    board.at({0, 0}).vampires = 5;

    return board;
}

void testInvalidConfiguration() {
    OpponentHistory invalidSpecies(
        Species::Unknown
    );

    assert(!invalidSpecies.isConfigured());

    const HistoryUpdateResult result =
        invalidSpecies.recordObservation(makeBoard(), 1);

    assert(!result.valid());
    assert(
        result.error ==
        OpponentHistoryError::InvalidConfiguration
    );

    OpponentHistory zeroCapacity(
        Species::Werewolves,
        0
    );

    assert(!zeroCapacity.isConfigured());
}

void testRecordsObservedPositions() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Werewolves);

    const HistoryUpdateResult result =
        history.recordObservation(board, 1);

    assert(result.valid());
    assert(result.positionsObserved == 2);
    assert(result.snapshotsRetained == 1);

    assert(history.snapshotCount() == 1);
    assert(history.latestTurn() == 1);

    const OpponentSnapshot& snapshot =
        history.snapshots().back();

    assert(snapshot.turn == 1);
    assert(snapshot.totalPopulation == 5);
    assert(snapshot.positions.size() == 2);

    assert((snapshot.positions[0].position == Position{1, 1}));
    assert((snapshot.positions[1].position == Position{3, 2}));

    assert((snapshot.positions[1].position == Position{3, 2}));
    assert(snapshot.positions[1].population == 3);
}

void testDoesNotRecordOurSpecies() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Werewolves);

    const HistoryUpdateResult result =
        history.recordObservation(board, 1);

    assert(result.valid());

    for (const OpponentPositionObservation& observation :
         history.snapshots().back().positions) {
        assert(
            (observation.position != Position{0, 0})
        );
    }
}

void testPositionStatistics() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    // Turn 1: population 2 at (1, 1).
    assert(history.recordObservation(board, 1).valid());

    // Turn 2: population increases to 3 at the same position.
    board.at({1, 1}).werewolves = 3;
    assert(history.recordObservation(board, 2).valid());

    // Turn 3: group is no longer observed at (1, 1).
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

    assert(std::abs(stats.presenceFrequency - (2.0 / 3.0)) < 1e-9);

    // Weights for turns 1, 2 and 3 are 0.25, 0.5 and 1.
    // Position (1, 1) was occupied on turns 1 and 2.
    // Its weighted frequency is therefore 0.75 / 1.75.
    const double expectedWeightedFrequency = 0.75 / 1.75;

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

void testPositionThatWasNeverObserved() {
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
    assert(result.statistics.latestPopulation == 0);
    assert(!result.statistics.hasBeenObserved);
}

void testSnapshotChangesDoNotInferMovement() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    // The group disappears from (1, 1) and appears at (2, 1).
    // We deliberately do not interpret this as a confirmed move.
    board.at({1, 1}).werewolves = 0;
    board.at({2, 1}).werewolves = 2;

    assert(history.recordObservation(board, 2).valid());

    assert(history.confirmedMovements().empty());
}

void testRecordsConfirmedMovement() {
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

void testRejectsInvalidConfirmedMovement() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    // Out-of-bounds destination.
    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {4, 1}, 2
        ) == OpponentHistoryError::InvalidPosition
    );

    // Non-adjacent destination.
    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {3, 3}, 2
        ) == OpponentHistoryError::InvalidMovement
    );

    // Same source and destination.
    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {1, 1}, 2
        ) == OpponentHistoryError::InvalidMovement
    );

    // Zero population.
    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {2, 1}, 0
        ) == OpponentHistoryError::InvalidMovement
    );

    // MOV count must fit in one byte.
    assert(
        history.recordConfirmedMovement(
            1, {1, 1}, {2, 1}, 256
        ) == OpponentHistoryError::InvalidMovement
    );

    assert(history.confirmedMovements().empty());
}

void testRejectsOutOfOrderMovement() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    assert(
        history.recordConfirmedMovement(
            3, {1, 1}, {2, 1}, 2
        ) == OpponentHistoryError::None
    );

    // An older confirmed movement must not be appended afterward.
    assert(
        history.recordConfirmedMovement(
            2, {1, 1}, {1, 2}, 1
        ) == OpponentHistoryError::InvalidTurn
    );
}

void testRejectsDuplicateAndOlderSnapshots() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 5).valid());

    const HistoryUpdateResult duplicate =
        history.recordObservation(board, 5);

    assert(!duplicate.valid());
    assert(duplicate.error == OpponentHistoryError::InvalidTurn);

    const HistoryUpdateResult older =
        history.recordObservation(board, 4);

    assert(!older.valid());
    assert(older.error == OpponentHistoryError::InvalidTurn);

    assert(history.snapshotCount() == 1);
}

void testRetainsNewestSnapshots() {
    OpponentHistory history(
        Species::Werewolves,
        2
    );

    BoardState board = makeBoard();

    board.at({3, 2}).werewolves = 0;

    assert(history.recordObservation(board, 1).valid());

    board.at({1, 1}).werewolves = 0;
    board.at({2, 2}).werewolves = 4;

    assert(history.recordObservation(board, 2).valid());

    board.at({1, 1}).werewolves = 3;
    board.at({2, 2}).werewolves = 0;

    assert(history.recordObservation(board, 3).valid());

    assert(history.snapshotCount() == 2);
    assert(history.snapshots()[0].turn == 2);
    assert(history.snapshots()[1].turn == 3);

    const PositionStatisticsResult result =
        history.getPositionStatistics(
            {1, 1},
            3
        );

    assert(result.valid());
    assert(result.statistics.snapshotsConsidered == 2);
    assert(result.statistics.presenceCount == 1);
    assert(std::abs(result.statistics.presenceFrequency - 0.5) < 1e-9);
}

void testRejectsChangedBoardDimensions() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 1).valid());

    BoardState changedBoard;
    changedBoard.rows = 5;
    changedBoard.columns = 4;
    changedBoard.cells.resize(20);
    changedBoard.ourSpecies = Species::Vampires;

    const HistoryUpdateResult result =
        history.recordObservation(changedBoard, 2);

    assert(!result.valid());
    assert(
        result.error ==
        OpponentHistoryError::BoardDimensionsChanged
    );

    assert(history.snapshotCount() == 1);
}

void testRejectsWrongOpponentSpecies() {
    OpponentHistory history(Species::Vampires);

    BoardState board = makeBoard();
    // board.ourSpecies is Vampires, so the opponent is Werewolves.

    const HistoryUpdateResult result =
        history.recordObservation(board, 1);

    assert(!result.valid());
    assert(result.error == OpponentHistoryError::InvalidSpecies);
}

void testRejectsInvalidStatisticsRequests() {
    OpponentHistory history(Species::Werewolves);

    BoardState board = makeBoard();

    assert(history.recordObservation(board, 5).valid());

    const PositionStatisticsResult invalidPosition =
        history.getPositionStatistics({4, 4}, 5);

    assert(!invalidPosition.valid());
    assert(
        invalidPosition.error ==
        OpponentHistoryError::InvalidPosition
    );

    const PositionStatisticsResult invalidHalfLife =
        history.getPositionStatistics(
            {1, 1},
            5,
            0.0
        );

    assert(!invalidHalfLife.valid());
    assert(
        invalidHalfLife.error ==
        OpponentHistoryError::InvalidHalfLife
    );

    const PositionStatisticsResult invalidTurn =
        history.getPositionStatistics(
            {1, 1},
            4
        );

    assert(!invalidTurn.valid());
    assert(invalidTurn.error == OpponentHistoryError::InvalidTurn);
}

void testConfirmedMovementRequiresInitialObservation() {
    OpponentHistory history(Species::Werewolves);

    const OpponentHistoryError error =
        history.recordConfirmedMovement(
            1,
            {1, 1},
            {2, 1},
            2
        );

    assert(error == OpponentHistoryError::HistoryNotInitialized);
}

} // namespace

int main() {
    testInvalidConfiguration();
    testRecordsObservedPositions();
    testDoesNotRecordOurSpecies();
    testPositionStatistics();
    testPositionThatWasNeverObserved();
    testSnapshotChangesDoNotInferMovement();
    testRecordsConfirmedMovement();
    testRejectsInvalidConfirmedMovement();
    testRejectsOutOfOrderMovement();
    testRejectsDuplicateAndOlderSnapshots();
    testRetainsNewestSnapshots();
    testRejectsChangedBoardDimensions();
    testRejectsWrongOpponentSpecies();
    testRejectsInvalidStatisticsRequests();
    testConfirmedMovementRequiresInitialObservation();

    std::cout << "All opponent history tests passed.\n";
    return 0;
}
