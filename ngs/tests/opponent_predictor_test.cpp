
#include "../src/opponent_predictor.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace vvw::ngs;

namespace {

BoardState makeBoard() {
    BoardState board;

    board.rows = 5;
    board.columns = 5;
    board.cells.resize(25);

    board.ourSpecies = Species::Vampires;

    // One opponent group.
    board.at({1, 2}).werewolves = 5;

    // A human house to the east.
    board.at({4, 2}).humans = 10;
    board.at({4, 2}).isHouse = true;

    return board;
}

OpponentHistory makeHistory(const BoardState& board) {
    OpponentHistory history(Species::Werewolves);

    // Three observations with the same reconstructed board.
    assert(history.recordObservation(board, 1).valid());

    // Protocol-confirmed eastward movements.
    assert(
        history.recordConfirmedMovement(
            1, {0, 0}, {1, 0}, 2
        ) == OpponentHistoryError::None
    );

    assert(history.recordObservation(board, 2).valid());

    assert(
        history.recordConfirmedMovement(
            2, {1, 0}, {2, 0}, 2
        ) == OpponentHistoryError::None
    );

    assert(history.recordObservation(board, 3).valid());

    assert(
        history.recordConfirmedMovement(
            3, {2, 0}, {3, 0}, 2
        ) == OpponentHistoryError::None
    );

    return history;
}

const DirectionPrediction& findDirection(
    const OpponentPredictionResult& result,
    int dx,
    int dy
) {
    for (const DirectionPrediction& direction : result.directions) {
        if (direction.dx == dx && direction.dy == dy) {
            return direction;
        }
    }

    assert(false && "Expected direction was not found");

    return result.directions.front();
}

const PredictedDestination* findDestination(
    const OpponentPredictionResult& result,
    Position source,
    Position destination
) {
    for (const PredictedDestination& prediction : result.destinations) {
        if (prediction.source == source &&
            prediction.destination == destination) {
            return &prediction;
        }
    }

    return nullptr;
}

void testInvalidBoard() {
    BoardState board;

    OpponentHistory history(Species::Werewolves);

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            1
        );

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::InvalidBoard);
}

void testInvalidSpecies() {
    BoardState board = makeBoard();

    OpponentHistory history;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            1
        );

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::InvalidSpecies);
}

void testMismatchedOpponentSpecies() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Vampires);

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            1
        );

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::InvalidSpecies);
}

void testEmptyHistory() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Werewolves);

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            1
        );

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::EmptyHistory);
}

void testHistoryDimensionMismatch() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Werewolves);

    BoardState differentBoard = board;
    differentBoard.rows = 4;
    differentBoard.cells.resize(20);

    assert(history.recordObservation(differentBoard, 1).valid());

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            1
        );

    assert(!result.valid());
    assert(
        result.error ==
        OpponentPredictionError::HistoryBoardDimensionsMismatch
    );
}

void testInvalidTurn() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            2
        );

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::InvalidTurn);
}

void testInvalidOptions() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    OpponentPredictionOptions options;
    options.maxDestinationsPerGroup = 0;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3,
            options
        );

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::InvalidOptions);
}

void testNegativeWeightsRejected() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    OpponentPredictionOptions options;
    options.directionWeight = -0.1;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3,
            options
        );

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::InvalidOptions);
}

void testDirectionStatisticsReflectConfirmedMovements() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3
        );

    assert(result.valid());

    assert(result.stats.movementObservationsUsed == 3);
    assert(result.directions.size() == 8);

    const DirectionPrediction& east =
        findDirection(result, 1, 0);

    const DirectionPrediction& west =
        findDirection(result, -1, 0);

    assert(east.observations == 3);
    assert(west.observations == 0);

    assert(east.probability > west.probability);
}

void testDirectionSmoothingProducesValidDistribution() {
    BoardState board = makeBoard();

    OpponentHistory history(Species::Werewolves);

    // No confirmed movement records.
    assert(history.recordObservation(board, 1).valid());

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            1
        );

    assert(result.valid());
    assert(result.directions.size() == 8);

    double totalProbability = 0.0;

    for (const DirectionPrediction& direction : result.directions) {
        totalProbability += direction.probability;

        assert(direction.observations == 0);
        assert(direction.probability > 0.0);
    }

    assert(std::abs(totalProbability - 1.0) < 1e-9);
}

void testHouseProgressInfluencesDestinationScore() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    OpponentPredictionOptions options;

    options.maxDestinationsPerGroup = 8;

    options.directionWeight = 0.0;
    options.destinationHistoryWeight = 0.0;
    options.houseProgressWeight = 1.0;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3,
            options
        );

    assert(result.valid());

    const PredictedDestination* east =
        findDestination(
            result,
            {1, 2},
            {2, 2}
        );

    const PredictedDestination* west =
        findDestination(
            result,
            {1, 2},
            {0, 2}
        );

    assert(east != nullptr);
    assert(west != nullptr);

    assert(east->houseProgressScore == 1.0);
    assert(west->houseProgressScore == 0.0);

    assert(east->priorityScore > west->priorityScore);
}

void testDestinationRankingIsBounded() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    OpponentPredictionOptions options;
    options.maxDestinationsPerGroup = 2;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3,
            options
        );

    assert(result.valid());
    assert(result.stats.opponentGroupsObserved == 1);

    assert(result.stats.groupsWithPredictions == 1);
    assert(result.destinations.size() <= 2);
}

void testPreviousPlyDestinationRestriction() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    board.previousPly.known = true;

    // This previous source cannot be a destination now.
    board.previousPly.sources.push_back({2, 1});

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3
        );

    assert(result.valid());

    assert(
        findDestination(result, {1, 2}, {2, 1}) == nullptr
    );

    assert(result.stats.destinationsRejectedByPreviousPly > 0);
}

void testPreviousPlySourceRestriction() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    board.previousPly.known = true;

    // A previous destination cannot become a current source.
    board.previousPly.destinations.push_back({1, 2});

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3
        );

    assert(result.valid());

    // The only opponent group is at (1,2).
    assert(result.stats.opponentGroupsObserved == 1);
    assert(result.stats.groupsWithPredictions == 0);
    assert(result.destinations.empty());
}

void testPredictedScoresAreBounded() {
    BoardState board = makeBoard();

    OpponentHistory history = makeHistory(board);

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3
        );

    assert(result.valid());

    for (const PredictedDestination& destination :
         result.destinations) {

        assert(destination.priorityScore >= 0.0);
        assert(destination.priorityScore <= 1.0);

        assert(destination.directionProbability >= 0.0);
        assert(destination.directionProbability <= 1.0);

        assert(
            destination.destinationPresenceFrequency >= 0.0
        );

        assert(
            destination.destinationPresenceFrequency <= 1.0
        );
    }
}

void testOriginalBoardIsNotModified() {
    BoardState board = makeBoard();

    const Population originalWerewolves =
        board.at({1, 2}).werewolves;

    const Population originalHumans =
        board.at({4, 2}).humans;

    OpponentHistory history = makeHistory(board);

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            board,
            history,
            3
        );

    assert(result.valid());

    assert(board.at({1, 2}).werewolves == originalWerewolves);
    assert(board.at({4, 2}).humans == originalHumans);
}

void testHouseTrendsAreReturned() {
    BoardState initialBoard = makeBoard();

    OpponentHistory history(Species::Werewolves);

    // Record the group approaching the house:
    // (1,2) -> (2,2) -> (3,2).
    for (std::uint32_t turn = 1; turn <= 3; ++turn) {
        BoardState historicalBoard = initialBoard;

        historicalBoard.at({1, 2}).werewolves = 0;
        historicalBoard.at({2, 2}).werewolves = 0;
        historicalBoard.at({3, 2}).werewolves = 0;

        historicalBoard.at({
            static_cast<int>(turn),
            2
        }).werewolves = 5;

        assert(
            history.recordObservation(
                historicalBoard,
                turn
            ).valid()
        );
    }

    // Current board must correspond to the latest observation.
    BoardState currentBoard = initialBoard;

    currentBoard.at({1, 2}).werewolves = 0;
    currentBoard.at({3, 2}).werewolves = 5;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(
            currentBoard,
            history,
            3
        );

    assert(result.valid());
    assert(result.houseTrends.size() == 1);

    assert(result.houseTrends.front().oldest_distance == 3);
    assert(result.houseTrends.front().newest_distance == 1);
    assert(result.houseTrends.front().risk > 0.0);
}

} // namespace

int main() {
    testInvalidBoard();
    testInvalidSpecies();
    testMismatchedOpponentSpecies();
    testEmptyHistory();
    testHistoryDimensionMismatch();
    testInvalidTurn();
    testInvalidOptions();
    testNegativeWeightsRejected();

    testDirectionStatisticsReflectConfirmedMovements();
    testDirectionSmoothingProducesValidDistribution();
    testHouseProgressInfluencesDestinationScore();
    testDestinationRankingIsBounded();

    testPreviousPlyDestinationRestriction();
    testPreviousPlySourceRestriction();

    testPredictedScoresAreBounded();
    testOriginalBoardIsNotModified();
    testHouseTrendsAreReturned();

    std::cout << "All opponent predictor tests passed.\n";

    return 0;
}
