#include "../src/candidate_generator.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

using namespace vvw::ngs;

namespace {

BoardState makeBoard() {
    BoardState board;
    board.rows = 5;
    board.columns = 5;
    board.cells.resize(25);
    board.ourSpecies = Species::Vampires;
    board.previousPly.known = true;
    return board;
}

std::vector<Population> countsForSourceAndDestination(
    const CandidateGenerationResult& result,
    Position source,
    Position destination
) {
    std::vector<Population> counts;
    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() == 1);
        const Move& move = plan.moves.front();
        if (move.source == source && move.destination == destination) {
            counts.push_back(move.count);
        }
    }
    return counts;
}

void testDefaultRatiosGenerateFourCountsPerNeighbour() {
    BoardState board = makeBoard();
    board.at({2, 2}).vampires = 100;

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    assert(result.stats.sourceGroupsExamined == 1);
    assert(result.stats.neighbourCellsExamined == 8);
    assert(result.candidates.size() == 32);

    const std::vector<Population> expected{25, 50, 75, 100};
    assert(countsForSourceAndDestination(result, {2, 2}, {3, 2}) == expected);
}

void testCornerSourceHasOnlyThreeNeighbours() {
    BoardState board = makeBoard();
    board.at({0, 0}).vampires = 100;

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    assert(result.stats.neighbourCellsExamined == 3);
    assert(result.candidates.size() == 12);
}

void testSmallPopulationRoundingAndDeduplication() {
    BoardState board = makeBoard();
    board.at({2, 2}).vampires = 3;

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    const std::vector<Population> expected{1, 2, 3};
    assert(countsForSourceAndDestination(result, {2, 2}, {3, 2}) == expected);
    assert(result.stats.duplicateCountsSkipped == 1);
    assert(result.candidates.size() == 24);
}

void testCountsRespectOneByteWireLimit() {
    BoardState board = makeBoard();
    board.at({2, 2}).vampires = 1000;

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    const std::vector<Population> expected{250, 255};
    assert(countsForSourceAndDestination(result, {2, 2}, {3, 2}) == expected);

    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.front().count <= 255);
    }
}

void testEveryCandidateContainsExactlyOneMoveAndIsValid() {
    BoardState board = makeBoard();
    board.at({1, 1}).vampires = 20;
    board.at({3, 3}).vampires = 40;

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    assert(!result.candidates.empty());

    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() == 1);
        assert(validateTurnPlan(board, plan, Species::Vampires).valid());
    }
}

void testPreviousPlyRestrictionsAreApplied() {
    BoardState board = makeBoard();
    board.at({2, 2}).vampires = 100;

    // A previous source cannot become a current destination.
    board.previousPly.sources.push_back({3, 2});

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    assert(countsForSourceAndDestination(result, {2, 2}, {3, 2}).empty());
    assert(result.stats.candidatesRejectedByValidator == 4);
}

void testPreviousDestinationCannotBeUsedAsSource() {
    BoardState board = makeBoard();
    board.at({2, 2}).vampires = 100;
    board.previousPly.destinations.push_back({2, 2});

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    assert(result.candidates.empty());
}

void testMissingPreviousFootprintIsReported() {
    BoardState board = makeBoard();
    board.at({2, 2}).vampires = 100;
    board.previousPly.known = false;

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(!result.valid());
    assert(result.error ==
           CandidateGenerationError::MissingPreviousPlyFootprint);
}

void testMissingPreviousFootprintCanBeAllowedForIsolatedTests() {
    BoardState board = makeBoard();
    board.at({2, 2}).vampires = 100;
    board.previousPly.known = false;

    CandidateGenerationOptions options;
    options.requirePreviousPlyFootprint = false;

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.candidates.size() == 32);
}

void testInvalidBoardIsReported() {
    BoardState board = makeBoard();
    board.cells.pop_back();

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(!result.valid());
    assert(result.error == CandidateGenerationError::InvalidBoard);
}

void testInvalidSpeciesIsReported() {
    BoardState board = makeBoard();

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Unknown);

    assert(!result.valid());
    assert(result.error == CandidateGenerationError::InvalidSpecies);
}

void testInvalidSplitRatioIsReported() {
    BoardState board = makeBoard();
    CandidateGenerationOptions options;
    options.splitRatios = {{0, 4}};

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires, options);

    assert(!result.valid());
    assert(result.error == CandidateGenerationError::InvalidSplitRatio);
}

void testNoGroupsProducesEmptyButValidResult() {
    BoardState board = makeBoard();

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    assert(result.candidates.empty());
    assert(result.stats.sourceGroupsExamined == 0);
}

} // namespace

int main() {
    testDefaultRatiosGenerateFourCountsPerNeighbour();
    testCornerSourceHasOnlyThreeNeighbours();
    testSmallPopulationRoundingAndDeduplication();
    testCountsRespectOneByteWireLimit();
    testEveryCandidateContainsExactlyOneMoveAndIsValid();
    testPreviousPlyRestrictionsAreApplied();
    testPreviousDestinationCannotBeUsedAsSource();
    testMissingPreviousFootprintIsReported();
    testMissingPreviousFootprintCanBeAllowedForIsolatedTests();
    testInvalidBoardIsReported();
    testInvalidSpeciesIsReported();
    testInvalidSplitRatioIsReported();
    testNoGroupsProducesEmptyButValidResult();

    std::cout << "All candidate generator tests passed.\n";
    return 0;
}
