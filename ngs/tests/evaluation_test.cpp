#include "../src/evaluation.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using namespace vvw::ngs;

namespace {

BoardState makeBoard(int rows, int columns) {
    BoardState board;
    board.rows = rows;
    board.columns = columns;
    board.cells.resize(static_cast<std::size_t>(rows * columns));
    board.ourSpecies = Species::Vampires;
    return board;
}

Cell& cell(BoardState& board, int x, int y) {
    return board.at(Position{x, y});
}

void testInvalidBoard() {
    BoardState board;
    const EvaluationResult result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves);
    assert(!result.valid());
    assert(result.error == EvaluationError::InvalidBoard);
}

void testInvalidSpecies() {
    BoardState board = makeBoard(3, 3);
    const EvaluationResult badPerspective = evaluatePosition(
        board, Species::Unknown, Species::Werewolves);
    assert(badPerspective.error == EvaluationError::InvalidPerspective);

    const EvaluationResult badSide = evaluatePosition(
        board, Species::Vampires, Species::Unknown);
    assert(badSide.error == EvaluationError::InvalidSideToMove);
}

void testPopulationAdvantageAndPerspectiveSymmetry() {
    BoardState board = makeBoard(3, 3);
    cell(board, 0, 0).vampires = 10;
    cell(board, 2, 2).werewolves = 4;

    EvaluationWeights weights;
    weights.humanPotential = 0.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 0.0;

    const auto vampireView = evaluatePosition(
        board, Species::Vampires, Species::Vampires, weights);
    const auto werewolfView = evaluatePosition(
        board, Species::Werewolves, Species::Vampires, weights);

    assert(vampireView.valid());
    assert(werewolfView.valid());
    assert(vampireView.features.populationAdvantage == 6.0);
    assert(werewolfView.features.populationAdvantage == -6.0);
    assert(vampireView.features.totalScore == 6.0);
    assert(werewolfView.features.totalScore == -6.0);
}

void testHumanPotentialFavoursCloserSpecies() {
    BoardState board = makeBoard(5, 5);
    cell(board, 0, 0).vampires = 5;
    cell(board, 4, 4).werewolves = 5;
    cell(board, 1, 0).humans = 8;

    EvaluationWeights weights;
    weights.population = 0.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 0.0;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);

    assert(result.valid());
    assert(result.features.humanPotentialAdvantage > 0.0);
}

void testNoThreatWithoutAdjacentEnemy() {
    BoardState board = makeBoard(5, 5);
    cell(board, 0, 0).vampires = 10;
    cell(board, 4, 4).werewolves = 10;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves);

    assert(result.valid());
    assert(result.features.expectedThreatenedPopulation == 0.0);
    assert(result.features.threatAdvantage == 0.0);
}

void testThreatSignDependsOnSideToMove() {
    BoardState board = makeBoard(3, 3);
    cell(board, 1, 1).vampires = 4;
    cell(board, 0, 0).werewolves = 4;

    EvaluationWeights weights;
    weights.population = 0.0;
    weights.humanPotential = 0.0;
    weights.fragmentation = 0.0;

    const auto vampiresMove = evaluatePosition(
        board, Species::Werewolves, Species::Vampires, weights);
    const auto werewolvesMove = evaluatePosition(
        board, Species::Werewolves, Species::Werewolves, weights);

    assert(vampiresMove.valid());
    assert(werewolvesMove.valid());
    assert(vampiresMove.features.threatAdvantage < 0.0);
    assert(werewolvesMove.features.threatAdvantage > 0.0);
}

void testPreviousPlyFootprintBlocksThreatEstimate() {
    BoardState board = makeBoard(3, 3);
    const Position defended{1, 1};
    const Position attacker{0, 0};
    cell(board, defended.x, defended.y).werewolves = 4;
    cell(board, attacker.x, attacker.y).vampires = 4;

    const auto unblocked = evaluatePosition(
        board, Species::Werewolves, Species::Vampires);
    assert(unblocked.features.expectedThreatenedPopulation > 0.0);

    board.previousPly.known = true;
    board.previousPly.sources.push_back(defended);
    const auto targetBlocked = evaluatePosition(
        board, Species::Werewolves, Species::Vampires);
    assert(targetBlocked.features.expectedThreatenedPopulation == 0.0);

    board.previousPly.sources.clear();
    board.previousPly.destinations.push_back(attacker);
    const auto sourceBlocked = evaluatePosition(
        board, Species::Werewolves, Species::Vampires);
    assert(sourceBlocked.features.expectedThreatenedPopulation == 0.0);
}

void testSmallDisconnectedGroupGetsFragmentationPenalty() {
    BoardState board = makeBoard(5, 5);
    cell(board, 0, 0).vampires = 10;
    cell(board, 4, 4).vampires = 1;
    cell(board, 2, 2).werewolves = 10;

    EvaluationWeights weights;
    weights.population = 0.0;
    weights.humanPotential = 0.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 1.0;

    const auto fragmented = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(fragmented.valid());
    assert(fragmented.features.ownFragmentationPenalty == 1.0);

    cell(board, 4, 4).vampires = 0;
    cell(board, 1, 0).vampires = 1; // 8-neighbour connected to the main group.
    const auto consolidated = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(consolidated.features.ownFragmentationPenalty == 0.0);
}


void testUnknownHorizonPreservesLegacyWeights() {
    BoardState board = makeBoard(2, 2);
    cell(board, 0, 0).vampires = 10;
    cell(board, 1, 1).werewolves = 4;

    EvaluationWeights weights;
    weights.humanPotential = 0.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 0.0;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);

    assert(result.valid());
    assert(!result.features.horizonAware);
    assert(result.features.endgameUrgency == 0.0);
    assert(result.features.effectivePopulationWeight == weights.population);
    assert(result.features.totalScore == 6.0);
}

void testEarlyHorizonKeepsBaseWeights() {
    BoardState board = makeBoard(2, 2);
    board.horizon.known = true;
    board.horizon.elapsedSeconds = 0.0;
    board.horizon.ourPliesUsed = 0;
    board.horizon.opponentPliesUsed = 0;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves);

    assert(result.valid());
    assert(result.features.horizonAware);
    assert(result.features.remainingHorizonFraction == 1.0);
    assert(result.features.endgameUrgency == 0.0);
    assert(result.features.effectivePopulationWeight == 1.0);
    assert(result.features.effectiveHumanPotentialWeight == 0.35);
    assert(result.features.effectiveThreatExposureWeight == 1.0);
    assert(result.features.effectiveFragmentationWeight == 0.20);
}

void testEndgameHorizonPrioritizesMaterialAndThreat() {
    BoardState board = makeBoard(2, 2);
    board.horizon.known = true;
    board.horizon.ourPliesUsed = GameHorizon::kMaxPliesPerSide;
    board.horizon.opponentPliesUsed = GameHorizon::kMaxPliesPerSide;
    board.horizon.elapsedSeconds = 120.0;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves);

    assert(result.valid());
    assert(result.features.horizonAware);
    assert(result.features.remainingHorizonFraction == 0.0);
    assert(result.features.endgameUrgency == 1.0);
    assert(result.features.effectivePopulationWeight == 1.5);
    assert(result.features.effectiveHumanPotentialWeight == 0.0875);
    assert(result.features.effectiveThreatExposureWeight == 1.4);
    assert(result.features.effectiveFragmentationWeight == 0.10);
}

void testTimeLimitAlsoTriggersEndgameAdaptation() {
    BoardState board = makeBoard(2, 2);
    board.horizon.known = true;
    board.horizon.ourPliesUsed = 10;
    board.horizon.opponentPliesUsed = 10;
    board.horizon.elapsedSeconds = 300.0;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves);

    assert(result.valid());
    assert(result.features.horizonAware);
    assert(result.features.endgameUrgency == 1.0);
}

void testInvalidHorizonFallsBackWithoutNan() {
    BoardState board = makeBoard(2, 2);
    board.horizon.known = true;
    board.horizon.elapsedSeconds = std::numeric_limits<double>::quiet_NaN();

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves);

    assert(result.valid());
    assert(!result.features.horizonAware);
    assert(result.features.endgameUrgency == 0.0);
    assert(std::isfinite(result.features.totalScore));
}

void testHorizonCountersBeyondLimitsAreClampedSafely() {
    BoardState board = makeBoard(2, 2);
    board.horizon.known = true;
    board.horizon.ourPliesUsed = static_cast<std::uint16_t>(
        GameHorizon::kMaxPliesPerSide + 5);
    board.horizon.opponentPliesUsed = static_cast<std::uint16_t>(
        GameHorizon::kMaxPliesPerSide + 2);
    board.horizon.elapsedSeconds = 100.0;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves);

    assert(result.valid());
    assert(result.features.horizonAware);
    assert(result.features.endgameUrgency == 1.0);
    assert(std::isfinite(result.features.totalScore));
}

void testHousePotentialRewardsContestedHouseAccess() {
    BoardState board = makeBoard(5, 5);
    cell(board, 0, 0).vampires = 10;
    cell(board, 4, 4).werewolves = 10;
    cell(board, 1, 0).humans = 8;
    cell(board, 1, 0).isHouse = true;

    EvaluationWeights weights;
    weights.population = 0.0;
    weights.humanPotential = 0.0;
    weights.housePotential = 1.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 0.0;

    const auto vampireView = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    const auto werewolfView = evaluatePosition(
        board, Species::Werewolves, Species::Vampires, weights);

    assert(vampireView.valid());
    assert(werewolfView.valid());
    assert(vampireView.features.housePotentialAdvantage > 0.0);
    assert(werewolfView.features.housePotentialAdvantage < 0.0);
    assert(std::abs(
        vampireView.features.housePotentialAdvantage +
        werewolfView.features.housePotentialAdvantage
    ) < 1e-9);
}

void testOrdinaryHumansDoNotReceiveHouseObjectiveBonus() {
    BoardState houseBoard = makeBoard(5, 5);
    cell(houseBoard, 0, 0).vampires = 10;
    cell(houseBoard, 4, 4).werewolves = 10;
    cell(houseBoard, 1, 0).humans = 8;
    cell(houseBoard, 1, 0).isHouse = true;

    BoardState ordinaryBoard = houseBoard;
    cell(ordinaryBoard, 1, 0).isHouse = false;

    EvaluationWeights weights;
    weights.population = 0.0;
    weights.humanPotential = 0.0;
    weights.housePotential = 1.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 0.0;

    const auto houseResult = evaluatePosition(
        houseBoard, Species::Vampires, Species::Werewolves, weights);
    const auto ordinaryResult = evaluatePosition(
        ordinaryBoard, Species::Vampires, Species::Werewolves, weights);

    assert(houseResult.valid());
    assert(ordinaryResult.valid());
    assert(houseResult.features.housePotentialAdvantage > 0.0);
    assert(ordinaryResult.features.housePotentialAdvantage == 0.0);
    assert(houseResult.features.totalScore > ordinaryResult.features.totalScore);
}


void testSmallGroupCannotMaskCaptureCapableHouseDistance() {
    BoardState board = makeBoard(7, 7);
    cell(board, 0, 0).vampires = 6;
    cell(board, 4, 4).vampires = 1; // Close, but cannot convert it deterministically.
    cell(board, 5, 4).humans = 6;
    cell(board, 5, 4).isHouse = true;

    EvaluationWeights weights;
    weights.population = 0.0;
    weights.humanPotential = 0.0;
    weights.housePotential = 1.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 0.0;

    const auto withSingleton = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(withSingleton.valid());

    // The eligible six-creature group is five Chebyshev steps away. The
    // nearby singleton must not make the six-human house look one step away.
    const double expected = 6.0 / 6.0;
    assert(std::abs(withSingleton.features.ownHousePotential - expected) < 1e-9);

    cell(board, 1, 1).vampires = 6;
    cell(board, 0, 0).vampires = 0;
    const auto mainGroupAdvanced = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(mainGroupAdvanced.valid());
    assert(mainGroupAdvanced.features.ownHousePotential >
           withSingleton.features.ownHousePotential);
}

void testReportedSelfPlayMapHasViableHouseGradient() {
    BoardState board = makeBoard(20, 20);
    cell(board, 19, 16).vampires = 20;
    cell(board, 1, 2).werewolves = 20;

    const Position houses[] = {
        {3, 14}, {6, 4}, {7, 15}, {12, 4}, {13, 15}, {16, 5}
    };
    const Population humans[] = {6, 37, 28, 28, 37, 6};
    for (std::size_t i = 0; i < 6; ++i) {
        cell(board, houses[i].x, houses[i].y).humans = humans[i];
        cell(board, houses[i].x, houses[i].y).isHouse = true;
    }

    EvaluationWeights weights;
    weights.population = 0.0;
    weights.humanPotential = 0.0;
    weights.housePotential = 1.0;
    weights.threatExposure = 0.0;
    weights.fragmentation = 0.0;

    const auto before = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(before.valid());

    // A 20-creature group can convert the two six-human houses
    // deterministically, but not the 28- or 37-human houses. Advancing
    // diagonally toward its viable house
    // should therefore improve its objective potential.
    cell(board, 19, 16).vampires = 0;
    cell(board, 18, 15).vampires = 20;
    const auto after = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(after.valid());
    assert(after.features.ownHousePotential > before.features.ownHousePotential);
}

void testNegativeHousePotentialWeightIsRejected() {
    BoardState board = makeBoard(2, 2);
    EvaluationWeights weights;
    weights.housePotential = -0.01;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(result.error == EvaluationError::InvalidWeights);
}

void testInvalidWeights() {
    BoardState board = makeBoard(2, 2);
    EvaluationWeights weights;
    weights.population = -1.0;

    const auto result = evaluatePosition(
        board, Species::Vampires, Species::Werewolves, weights);
    assert(result.error == EvaluationError::InvalidWeights);
}

} // namespace

int main() {
    testInvalidBoard();
    testInvalidSpecies();
    testPopulationAdvantageAndPerspectiveSymmetry();
    testHumanPotentialFavoursCloserSpecies();
    testNoThreatWithoutAdjacentEnemy();
    testThreatSignDependsOnSideToMove();
    testPreviousPlyFootprintBlocksThreatEstimate();
    testSmallDisconnectedGroupGetsFragmentationPenalty();
    testHousePotentialRewardsContestedHouseAccess();
    testOrdinaryHumansDoNotReceiveHouseObjectiveBonus();
    testSmallGroupCannotMaskCaptureCapableHouseDistance();
    testReportedSelfPlayMapHasViableHouseGradient();
    testNegativeHousePotentialWeightIsRejected();
    testUnknownHorizonPreservesLegacyWeights();
    testEarlyHorizonKeepsBaseWeights();
    testEndgameHorizonPrioritizesMaterialAndThreat();
    testTimeLimitAlsoTriggersEndgameAdaptation();
    testInvalidHorizonFallsBackWithoutNan();
    testHorizonCountersBeyondLimitsAreClampedSafely();
    testInvalidWeights();

    std::cout << "All evaluation tests passed.\n";
    return 0;
}
