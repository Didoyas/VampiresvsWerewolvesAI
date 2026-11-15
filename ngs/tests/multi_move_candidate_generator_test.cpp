#include "../src/multi_move_candidate_generator.hpp"

#include <cassert>
#include <cstddef>
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

bool hasDistinctSources(const TurnPlan& plan) {
    std::set<std::pair<int, int>> sources;
    for (const Move& move : plan.moves) {
        sources.insert({move.source.x, move.source.y});
    }
    return sources.size() == plan.moves.size();
}

void configureTwoSources(BoardState& board) {
    board.at({0, 0}).vampires = 100;
    board.at({4, 4}).vampires = 80;
}

void configureThreeSources(BoardState& board) {
    board.at({0, 0}).vampires = 100;
    board.at({4, 0}).vampires = 80;
    board.at({0, 4}).vampires = 60;
}

void testGeneratesTwoSourcePlans() {
    BoardState board = makeBoard();
    configureTwoSources(board);

    MultiMoveCandidateOptions options;
    options.maxMovesPerPlan = 2;
    options.maxSingleMoveCandidates = 8;
    options.maxTwoMoveCandidates = 4;
    options.maxThreeMoveCandidates = 0;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.stats.sourceGroupsAvailable == 2);
    assert(result.stats.twoMoveCandidatesGenerated > 0);

    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() <= 2);
        assert(validateTurnPlan(board, plan, Species::Vampires).valid());
        if (plan.moves.size() == 2) {
            assert(hasDistinctSources(plan));
        }
    }
}

void testGeneratesThreeSourcePlans() {
    BoardState board = makeBoard();
    configureThreeSources(board);

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 0;
    options.maxTwoMoveCandidates = 4;
    options.maxThreeMoveCandidates = 4;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.stats.threeMoveCandidatesGenerated > 0);

    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() == 2 || plan.moves.size() == 3);
        assert(hasDistinctSources(plan));
        assert(validateTurnPlan(board, plan, Species::Vampires).valid());
    }
}

void testEveryCompositePlanUsesDifferentSourceCells() {
    BoardState board = makeBoard();
    board.at({1, 1}).vampires = 100;
    board.at({3, 1}).vampires = 100;
    board.at({1, 3}).vampires = 100;
    board.at({3, 3}).vampires = 100;

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 0;
    options.maxTwoMoveCandidates = 20;
    options.maxThreeMoveCandidates = 20;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(!result.candidates.empty());

    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() >= 2);
        assert(hasDistinctSources(plan));
    }
}

void testSingleMoveFallbacksAreRetained() {
    BoardState board = makeBoard();
    configureTwoSources(board);

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 5;
    options.maxTwoMoveCandidates = 0;
    options.maxThreeMoveCandidates = 0;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.candidates.size() == 5);
    assert(result.stats.singleMoveCandidatesIncluded == 5);
    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() == 1);
    }
}

void testCandidateQuotasAreRespected() {
    BoardState board = makeBoard();
    configureThreeSources(board);

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 3;
    options.maxTwoMoveCandidates = 5;
    options.maxThreeMoveCandidates = 2;
    options.variantsPerSourceCombination = 4;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.stats.singleMoveCandidatesIncluded <= 3);
    assert(result.stats.twoMoveCandidatesGenerated <= 5);
    assert(result.stats.threeMoveCandidatesGenerated <= 2);
    assert(result.candidates.size() <= 10);
}

void testCombinationAttemptBudgetIsEnforced() {
    BoardState board = makeBoard();
    configureThreeSources(board);

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 0;
    options.maxTwoMoveCandidates = 100;
    options.maxThreeMoveCandidates = 100;
    options.variantsPerSourceCombination = 10;
    options.maxCombinationAttempts = 1;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.stats.combinationAttempts <= 1);
    assert(result.stats.combinationBudgetExhausted);
}

void testInvalidOptionsAreReported() {
    BoardState board = makeBoard();
    configureTwoSources(board);

    MultiMoveCandidateOptions options;
    options.maxMovesPerPlan = 4;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(!result.valid());
    assert(result.error == MultiMoveCandidateError::InvalidOptions);
}

void testStageOneErrorsArePropagated() {
    BoardState board = makeBoard();
    board.previousPly.known = false;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires);

    assert(!result.valid());
    assert(result.error ==
           MultiMoveCandidateError::MissingPreviousPlyFootprint);
}

void testGenerationIsDeterministic() {
    BoardState board = makeBoard();
    configureThreeSources(board);

    const MultiMoveCandidateResult first =
        generateMultiMoveCandidates(board, Species::Vampires);
    const MultiMoveCandidateResult second =
        generateMultiMoveCandidates(board, Species::Vampires);

    assert(first.valid() && second.valid());
    assert(first.candidates.size() == second.candidates.size());

    for (std::size_t i = 0; i < first.candidates.size(); ++i) {
        assert(first.candidates[i].moves.size() ==
               second.candidates[i].moves.size());
        for (std::size_t j = 0; j < first.candidates[i].moves.size(); ++j) {
            const Move& a = first.candidates[i].moves[j];
            const Move& b = second.candidates[i].moves[j];
            assert(a.source == b.source);
            assert(a.destination == b.destination);
            assert(a.count == b.count);
        }
    }
}

void testNoAvailableGroupsProducesValidEmptyResult() {
    BoardState board = makeBoard();

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires);

    assert(result.valid());
    assert(result.candidates.empty());
    assert(result.stats.sourceGroupsAvailable == 0);
}

} // namespace

int main() {
    testGeneratesTwoSourcePlans();
    testGeneratesThreeSourcePlans();
    testEveryCompositePlanUsesDifferentSourceCells();
    testSingleMoveFallbacksAreRetained();
    testCandidateQuotasAreRespected();
    testCombinationAttemptBudgetIsEnforced();
    testInvalidOptionsAreReported();
    testStageOneErrorsArePropagated();
    testGenerationIsDeterministic();
    testNoAvailableGroupsProducesValidEmptyResult();

    std::cout << "All multi-move candidate generator tests passed.\n";
    return 0;
}
