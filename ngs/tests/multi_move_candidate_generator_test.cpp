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

void configureFourSources(BoardState& board) {
    board.at({0, 0}).vampires = 100;
    board.at({4, 0}).vampires = 80;
    board.at({0, 4}).vampires = 60;
    board.at({4, 4}).vampires = 40;
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

void testGeneratesFourSourcePlans() {
    BoardState board = makeBoard();
    configureFourSources(board);

    MultiMoveCandidateOptions options;
    options.maxMovesPerPlan = 4;
    options.maxSingleMoveCandidates = 0;
    options.maxTwoMoveCandidates = 0;
    options.maxThreeMoveCandidates = 0;
    options.maxFourMoveCandidates = 8;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.stats.sourceGroupsAvailable == 4);
    assert(result.stats.fourMoveCandidatesGenerated > 0);

    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() == 4);
        assert(hasDistinctSources(plan));
        assert(validateTurnPlan(board, plan, Species::Vampires).valid());
    }
}

void testFourMoveCandidateQuotaIsRespected() {
    BoardState board = makeBoard();
    configureFourSources(board);

    MultiMoveCandidateOptions options;
    options.maxMovesPerPlan = 4;
    options.maxSingleMoveCandidates = 0;
    options.maxTwoMoveCandidates = 0;
    options.maxThreeMoveCandidates = 0;
    options.maxFourMoveCandidates = 2;
    options.variantsPerSourceCombination = 4;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.stats.fourMoveCandidatesGenerated <= 2);
    assert(result.candidates.size() <= 2);

    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() == 4);
        assert(hasDistinctSources(plan));
    }
}

void testFourMoveCombinationAttemptBudgetIsEnforced() {
    BoardState board = makeBoard();
    configureFourSources(board);

    MultiMoveCandidateOptions options;
    options.maxMovesPerPlan = 4;
    options.maxSingleMoveCandidates = 0;
    options.maxTwoMoveCandidates = 0;
    options.maxThreeMoveCandidates = 0;
    options.maxFourMoveCandidates = 100;
    options.variantsPerSourceCombination = 10;
    options.maxCombinationAttempts = 1;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.stats.combinationAttempts <= 1);
    assert(result.stats.combinationBudgetExhausted);
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
    options.maxMovesPerPlan = 5;

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


int chebyshevDistanceForTest(Position lhs, Position rhs) {
    const int dx = lhs.x > rhs.x ? lhs.x - rhs.x : rhs.x - lhs.x;
    const int dy = lhs.y > rhs.y ? lhs.y - rhs.y : rhs.y - lhs.y;
    return dx > dy ? dx : dy;
}

void testHighPriorityHouseApproachSurvivesSingleMoveQuota() {
    BoardState board;
    board.rows = 10;
    board.columns = 10;
    board.cells.resize(100);
    board.ourSpecies = Species::Vampires;
    board.previousPly.known = true;

    // Put several low-population groups earlier in row-major order. None can
    // capture the house, so they must not displace the main army's useful move.
    board.at({0, 0}).vampires = 2;
    board.at({0, 2}).vampires = 2;
    board.at({0, 4}).vampires = 2;
    board.at({0, 6}).vampires = 2;
    board.at({0, 8}).vampires = 2;

    // This source appears later in row-major order and can capture the house.
    board.at({5, 5}).vampires = 20;
    board.at({8, 8}).humans = 6;
    board.at({8, 8}).isHouse = true;

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 1;
    options.maxTwoMoveCandidates = 0;
    options.maxThreeMoveCandidates = 0;
    options.maxFourMoveCandidates = 0;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.candidates.size() == 1);
    assert(result.candidates.front().moves.size() == 1);

    const Move& selected = result.candidates.front().moves.front();
    assert((selected.source == Position{5, 5}));
    assert(selected.count >= 6);
    assert(chebyshevDistanceForTest(selected.destination, {8, 8}) <
           chebyshevDistanceForTest(selected.source, {8, 8}));
    assert(result.stats.singleMoveSourceGroupsCovered == 1);
}

void testSingleMoveShortlistCoversDistinctGroupsBeforeSecondChoices() {
    BoardState board = makeBoard();
    board.at({0, 0}).vampires = 8;
    board.at({4, 0}).vampires = 12;
    board.at({0, 4}).vampires = 10;

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 3;
    options.maxTwoMoveCandidates = 0;
    options.maxThreeMoveCandidates = 0;
    options.maxFourMoveCandidates = 0;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.candidates.size() == 3);

    std::set<std::pair<int, int>> sources;
    for (const TurnPlan& plan : result.candidates) {
        assert(plan.moves.size() == 1);
        sources.insert({plan.moves.front().source.x,
                        plan.moves.front().source.y});
    }

    assert(sources.size() == 3);
    assert(result.stats.singleMoveSourceGroupsCovered == 3);
}

void testBestDirectionIsSelectedForViableHouse() {
    BoardState board = makeBoard();
    board.at({1, 1}).vampires = 20;
    board.at({3, 3}).humans = 6;
    board.at({3, 3}).isHouse = true;

    MultiMoveCandidateOptions options;
    options.maxSingleMoveCandidates = 1;
    options.maxTwoMoveCandidates = 0;
    options.maxThreeMoveCandidates = 0;
    options.maxFourMoveCandidates = 0;

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(board, Species::Vampires, options);

    assert(result.valid());
    assert(result.candidates.size() == 1);

    const Move& selected = result.candidates.front().moves.front();
    assert((selected.source == Position{1, 1}));
    assert(selected.count >= 6);
    assert((selected.destination == Position{2, 2}));
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
    testGeneratesFourSourcePlans();
    testFourMoveCandidateQuotaIsRespected();
    testFourMoveCombinationAttemptBudgetIsEnforced();
    testEveryCompositePlanUsesDifferentSourceCells();
    testSingleMoveFallbacksAreRetained();
    testCandidateQuotasAreRespected();
    testCombinationAttemptBudgetIsEnforced();
    testInvalidOptionsAreReported();
    testStageOneErrorsArePropagated();
    testGenerationIsDeterministic();
    testHighPriorityHouseApproachSurvivesSingleMoveQuota();
    testSingleMoveShortlistCoversDistinctGroupsBeforeSecondChoices();
    testBestDirectionIsSelectedForViableHouse();
    testNoAvailableGroupsProducesValidEmptyResult();

    std::cout << "All multi-move candidate generator tests passed.\n";
    return 0;
}