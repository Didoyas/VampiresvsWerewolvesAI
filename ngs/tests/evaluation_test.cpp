#include "../src/evaluation.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

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
    testInvalidWeights();

    std::cout << "All evaluation tests passed.\n";
    return 0;
}
