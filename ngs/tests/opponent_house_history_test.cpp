#include "../src/opponent_predictor.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace vvw::ngs;

namespace {

BoardState makeBoard() {
    BoardState board;
    board.rows = 7;
    board.columns = 7;
    board.cells.resize(49);
    board.ourSpecies = Species::Vampires;
    board.previousPly.known = false;

    // Two active houses. The opponent will approach only the eastern house.
    board.at({5, 3}).humans = 4;
    board.at({5, 3}).isHouse = true;

    board.at({2, 0}).humans = 4;
    board.at({2, 0}).isHouse = true;

    return board;
}

const PredictedDestination* findPrediction(
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

OpponentPredictionResult makeHistoryBiasedPrediction() {
    OpponentHistory history(Species::Werewolves);

    BoardState first = makeBoard();
    first.at({1, 3}).werewolves = 9;
    assert(history.recordObservation(first, 1).valid());

    BoardState current = makeBoard();
    current.at({2, 3}).werewolves = 9;
    assert(history.recordObservation(current, 2).valid());

    OpponentPredictionOptions options;
    options.directionWeight = 0.0;
    options.destinationHistoryWeight = 0.0;
    options.houseProgressWeight = 0.0;
    options.houseHistoryWeight = 1.0;
    options.maxDestinationsPerGroup = 8;

    return predictOpponentBehaviour(current, history, 2, options);
}

void testHistoricalHouseApproachInfluencesDestinationPriority() {
    const OpponentPredictionResult result =
        makeHistoryBiasedPrediction();

    assert(result.valid());
    assert(result.stats.houseTrendsAvailable == 1);
    assert(result.stats.destinationsWithHouseHistorySupport > 0);

    const PredictedDestination* towardHistoricallyApproachedHouse =
        findPrediction(result, {2, 3}, {3, 3});
    const PredictedDestination* towardOtherHouse =
        findPrediction(result, {2, 3}, {2, 2});

    assert(towardHistoricallyApproachedHouse != nullptr);
    assert(towardOtherHouse != nullptr);

    assert(std::abs(
        towardHistoricallyApproachedHouse->houseHistoryScore - 1.0
    ) < 1e-9);
    assert(std::abs(towardOtherHouse->houseHistoryScore) < 1e-9);
    assert(towardHistoricallyApproachedHouse->priorityScore >
           towardOtherHouse->priorityScore);
}

void testOneSnapshotDoesNotCreateHouseHistoryEvidence() {
    OpponentHistory history(Species::Werewolves);
    BoardState board = makeBoard();
    board.at({2, 3}).werewolves = 9;

    assert(history.recordObservation(board, 1).valid());

    const OpponentPredictionResult result =
        predictOpponentBehaviour(board, history, 1);

    assert(result.valid());
    assert(result.stats.houseTrendsAvailable == 0);
    assert(result.stats.destinationsWithHouseHistorySupport == 0);

    for (const PredictedDestination& prediction : result.destinations) {
        assert(std::abs(prediction.houseHistoryScore) < 1e-9);
    }
}

void testZeroRiskHouseTrendsDoNotDiluteExistingSignals() {
    OpponentHistory history(Species::Werewolves);
    BoardState board = makeBoard();
    board.at({2, 3}).werewolves = 9;

    assert(history.recordObservation(board, 1).valid());
    assert(history.recordObservation(board, 2).valid());

    OpponentPredictionOptions withHistoryWeight;
    withHistoryWeight.houseHistoryWeight = 1.0;

    OpponentPredictionOptions withoutHistoryWeight = withHistoryWeight;
    withoutHistoryWeight.houseHistoryWeight = 0.0;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(board, history, 2, withHistoryWeight);
    const OpponentPredictionResult baseline =
        predictOpponentBehaviour(board, history, 2, withoutHistoryWeight);

    assert(result.valid());
    assert(baseline.valid());
    assert(result.stats.houseTrendsAvailable == 0);
    assert(result.stats.destinationsWithHouseHistorySupport == 0);
    assert(result.destinations.size() == baseline.destinations.size());

    for (std::size_t i = 0; i < result.destinations.size(); ++i) {
        const PredictedDestination& prediction = result.destinations[i];
        const PredictedDestination& baselinePrediction = baseline.destinations[i];
        assert(std::abs(prediction.houseHistoryScore) < 1e-9);
        assert(std::isfinite(prediction.priorityScore));
        assert(prediction.source == baselinePrediction.source);
        assert(prediction.destination == baselinePrediction.destination);
        assert(std::abs(
            prediction.priorityScore - baselinePrediction.priorityScore
        ) < 1e-9);
    }
}

void testInvalidHouseHistoryWeightIsRejected() {
    OpponentHistory history(Species::Werewolves);
    BoardState board = makeBoard();
    board.at({2, 3}).werewolves = 9;
    assert(history.recordObservation(board, 1).valid());

    OpponentPredictionOptions options;
    options.houseHistoryWeight = -0.1;

    const OpponentPredictionResult result =
        predictOpponentBehaviour(board, history, 1, options);

    assert(!result.valid());
    assert(result.error == OpponentPredictionError::InvalidOptions);
}

} // namespace

int main() {
    testHistoricalHouseApproachInfluencesDestinationPriority();
    testOneSnapshotDoesNotCreateHouseHistoryEvidence();
    testZeroRiskHouseTrendsDoNotDiluteExistingSignals();
    testInvalidHouseHistoryWeightIsRejected();

    std::cout << "All opponent house-history integration tests passed.\n";
    return 0;
}
