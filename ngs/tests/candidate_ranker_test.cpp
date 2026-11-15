#include "../src/candidate_ranker.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace vvw::ngs;

namespace {

BoardState makeBoard() {
    BoardState board;

    board.rows = 7;
    board.columns = 7;
    board.cells.resize(49);

    board.ourSpecies = Species::Vampires;

    // Required by the movement validator.
    board.previousPly.known = true;

    // Our main group.
    board.at({1, 1}).vampires = 20;

    // Enemy group, far from the immediate action.
    board.at({6, 6}).werewolves = 8;

    // Human group near our vampires.
    board.at({2, 1}).humans = 5;

    return board;
}

TurnPlan makePlan(
    Position source,
    Population count,
    Position destination
) {
    TurnPlan plan;

    plan.moves.push_back(
        Move{source, count, destination}
    );

    return plan;
}

// -----------------------------------------------------------------------------
// Input validation
// -----------------------------------------------------------------------------

void testInvalidBoard() {
    BoardState board;

    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {2, 1})
    };

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires
    );

    assert(!result.valid());
    assert(result.error == CandidateRankingError::InvalidBoard);
}

void testInvalidSpecies() {
    const BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {2, 1})
    };

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Unknown
    );

    assert(!result.valid());
    assert(result.error == CandidateRankingError::InvalidSpecies);
}

void testEmptyCandidates() {
    const BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates;

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires
    );

    assert(!result.valid());
    assert(result.error == CandidateRankingError::EmptyCandidates);
}

void testInvalidOptions() {
    const BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {2, 1})
    };

    CandidateRankingOptions options;
    options.simulationsPerCandidate = 0;

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires,
        options
    );

    assert(!result.valid());
    assert(result.error == CandidateRankingError::InvalidOptions);
}

// -----------------------------------------------------------------------------
// Ranking behaviour
// -----------------------------------------------------------------------------

void testHumanConversionCandidateRanksHigher() {
    const BoardState board = makeBoard();

    // Candidate 0:
    // Move 10 vampires to the cell containing 5 humans.
    const TurnPlan captureHumans =
        makePlan({1, 1}, 10, {2, 1});

    // Candidate 1:
    // Move the same number of vampires to an empty cell.
    const TurnPlan moveToEmptyCell =
        makePlan({1, 1}, 10, {2, 2});

    const std::vector<TurnPlan> candidates{
        captureHumans,
        moveToEmptyCell
    };

    CandidateRankingOptions options;
    options.simulationsPerCandidate = 4;
    options.maxCandidatesToEvaluate = 10;

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires,
        options
    );

    assert(result.valid());
    assert(result.ranked.size() == 2);

    // Capturing the humans increases our total population.
    assert(result.ranked.front().candidateIndex == 0);

    assert(
        result.ranked[0].rankingScore >
        result.ranked[1].rankingScore
    );
}

void testOriginalBoardIsNotModified() {
    const BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {2, 1})
    };

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires
    );

    assert(result.valid());

    // Original board must remain unchanged.
    assert(board.at({1, 1}).vampires == 20);
    assert(board.at({2, 1}).humans == 5);
    assert(board.at({2, 1}).vampires == 0);
}

void testDeterministicCandidateUsesOneSimulation() {
    const BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {2, 1}),
        makePlan({1, 1}, 10, {2, 2})
    };

    CandidateRankingOptions options;
    options.simulationsPerCandidate = 4;

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires,
        options
    );

    assert(result.valid());

    // Neither movement causes a random battle:
    // 10 vampires are enough to convert 5 humans,
    // and the empty-cell movement is deterministic.
    assert(result.stats.deterministicCandidates == 2);
    assert(result.stats.stochasticCandidates == 0);

    // Each deterministic candidate should need only one simulation.
    assert(result.stats.simulationsAttempted == 2);
    assert(result.stats.simulationsSucceeded == 2);
}

void testRandomBattleUsesRepeatedSimulations() {
    BoardState board = makeBoard();

    // 10 attackers are fewer than 30 humans, so the transition
    // engine enters its random-battle path.
    board.at({2, 1}).humans = 30;

    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {2, 1})
    };

    CandidateRankingOptions options;
    options.simulationsPerCandidate = 6;

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires,
        options
    );

    assert(result.valid());
    assert(result.ranked.size() == 1);

    assert(result.stats.stochasticCandidates == 1);
    assert(result.ranked.front().simulationsCompleted == 6);
}

// -----------------------------------------------------------------------------
// Candidate budget
// -----------------------------------------------------------------------------

void testCandidateBudgetIsRespected() {
    const BoardState board = makeBoard();

    // All five movements are legal neighbours of (1,1).
    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {0, 0}),
        makePlan({1, 1}, 10, {1, 0}),
        makePlan({1, 1}, 10, {2, 0}),
        makePlan({1, 1}, 10, {0, 1}),
        makePlan({1, 1}, 10, {2, 1})
    };

    CandidateRankingOptions options;
    options.maxCandidatesToEvaluate = 2;

    const auto result = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires,
        options
    );

    assert(result.valid());

    assert(result.stats.candidatesProvided == 5);
    assert(result.stats.candidatesSelectedForEvaluation == 2);
    assert(result.stats.candidatesSkippedByBudget == 3);
    assert(result.ranked.size() == 2);
}

// -----------------------------------------------------------------------------
// Reproducibility
// -----------------------------------------------------------------------------

void testRandomSimulationIsReproducible() {
    BoardState board = makeBoard();
    board.at({2, 1}).humans = 30;

    const std::vector<TurnPlan> candidates{
        makePlan({1, 1}, 10, {2, 1})
    };

    CandidateRankingOptions options;
    options.simulationsPerCandidate = 8;
    options.baseRandomSeed = 12345;

    const auto first = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires,
        options
    );

    const auto second = rankTurnCandidates(
        board,
        candidates,
        Species::Vampires,
        options
    );

    assert(first.valid());
    assert(second.valid());

    assert(
        std::abs(
            first.ranked.front().meanScore -
            second.ranked.front().meanScore
        ) < 1e-9
    );

    assert(
        std::abs(
            first.ranked.front().standardDeviation -
            second.ranked.front().standardDeviation
        ) < 1e-9
    );
}

} // namespace

int main() {
    testInvalidBoard();
    testInvalidSpecies();
    testEmptyCandidates();
    testInvalidOptions();

    testHumanConversionCandidateRanksHigher();
    testOriginalBoardIsNotModified();
    testDeterministicCandidateUsesOneSimulation();
    testRandomBattleUsesRepeatedSimulations();

    testCandidateBudgetIsRespected();
    testRandomSimulationIsReproducible();

    std::cout << "All candidate ranker tests passed.\n";

    return 0;
}