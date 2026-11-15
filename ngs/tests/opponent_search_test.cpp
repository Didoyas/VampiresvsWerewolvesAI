
#include "../src/opponent_search.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
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

    // Our group.
    board.at({1, 1}).vampires = 4;

    // Opponent group.
    board.at({4, 4}).werewolves = 3;

    // Humans that our group can convert.
    board.at({2, 1}).humans = 3;

    return board;
}

TurnPlan makeConversionPlan() {
    TurnPlan plan;
    plan.moves.push_back({
        {1, 1},
        4,
        {2, 1}
    });
    return plan;
}

TurnPlan makeAlternativePlan() {
    TurnPlan plan;
    plan.moves.push_back({
        {1, 1},
        4,
        {0, 0}
    });
    return plan;
}

OpponentSearchOptions makeTestOptions() {
    OpponentSearchOptions options;

    options.maxRootCandidates = 2;
    options.maxOpponentResponses = 8;
    options.simulationsPerRootCandidate = 1;
    options.riskAversion = 0.0;

    options.rootRankingOptions.simulationsPerCandidate = 1;
    options.rootRankingOptions.maxCandidatesToEvaluate = 16;

    return options;
}

void testInvalidBoard() {
    BoardState board;

    std::vector<TurnPlan> candidates{
        makeConversionPlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires
        );

    assert(!result.valid());
    assert(result.error == OpponentSearchError::InvalidBoard);
}

void testInvalidSpecies() {
    BoardState board = makeBoard();

    std::vector<TurnPlan> candidates{
        makeConversionPlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Unknown
        );

    assert(!result.valid());
    assert(result.error == OpponentSearchError::InvalidSpecies);
}

void testEmptyCandidates() {
    BoardState board = makeBoard();
    std::vector<TurnPlan> candidates;

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires
        );

    assert(!result.valid());
    assert(result.error == OpponentSearchError::EmptyCandidates);
}

void testInvalidOptions() {
    BoardState board = makeBoard();

    std::vector<TurnPlan> candidates{
        makeConversionPlan()
    };

    OpponentSearchOptions options = makeTestOptions();
    options.maxOpponentResponses = 0;

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            options
        );

    assert(!result.valid());
    assert(result.error == OpponentSearchError::InvalidOptions);
}

void testSearchReturnsOriginalCandidateIndex() {
    BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makeConversionPlan(),
        makeAlternativePlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            makeTestOptions()
        );

    assert(result.valid());
    assert(result.hasBestCandidate);

    assert(result.bestCandidateIndex < candidates.size());
    assert(!result.ranked.empty());

    assert(
        result.bestCandidateIndex ==
        result.ranked.front().candidateIndex
    );
}

void testOpponentResponseBudgetIsRespected() {
    BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makeConversionPlan()
    };

    OpponentSearchOptions options = makeTestOptions();
    options.maxOpponentResponses = 2;

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            options
        );

    assert(result.valid());

    // There is one root candidate and one simulation.
    // No more than two opponent responses may be examined.
    assert(result.stats.opponentResponsesExamined <= 2);
}

void testRootCandidateBudgetIsRespected() {
    BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makeConversionPlan(),
        makeAlternativePlan()
    };

    OpponentSearchOptions options = makeTestOptions();
    options.maxRootCandidates = 1;

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            options
        );

    assert(result.valid());
    assert(result.stats.rootCandidatesShortlisted == 1);
    assert(result.ranked.size() <= 1);
}

void testNoOpponentResponseCanBeEvaluated() {
    BoardState board = makeBoard();

    // Remove the opponent group.
    board.at({4, 4}).werewolves = 0;

    const std::vector<TurnPlan> candidates{
        makeConversionPlan()
    };

    OpponentSearchOptions options = makeTestOptions();
    options.maxRootCandidates = 1;

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            options
        );

    assert(result.valid());
    assert(result.stats.simulationsWithoutOpponentResponse == 1);
    assert(result.stats.simulationsCompleted == 1);
}

void testOriginalBoardIsNotModified() {
    BoardState board = makeBoard();

    const Population originalVampires =
        board.at({1, 1}).vampires;
    const Population originalHumans =
        board.at({2, 1}).humans;
    const Population originalWerewolves =
        board.at({4, 4}).werewolves;

    const std::vector<TurnPlan> candidates{
        makeConversionPlan(),
        makeAlternativePlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            makeTestOptions()
        );

    assert(result.valid());

    assert(board.at({1, 1}).vampires == originalVampires);
    assert(board.at({2, 1}).humans == originalHumans);
    assert(board.at({4, 4}).werewolves == originalWerewolves);
}

void testSearchIsReproducible() {
    BoardState board = makeBoard();

    const std::vector<TurnPlan> candidates{
        makeConversionPlan(),
        makeAlternativePlan()
    };

    OpponentSearchOptions options = makeTestOptions();

    const OpponentSearchResult first =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            options
        );

    const OpponentSearchResult second =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            options
        );

    assert(first.valid());
    assert(second.valid());

    assert(first.bestCandidateIndex == second.bestCandidateIndex);
    assert(first.ranked.size() == second.ranked.size());

    for (std::size_t i = 0; i < first.ranked.size(); ++i) {
        assert(
            std::abs(
                first.ranked[i].rankingScore -
                second.ranked[i].rankingScore
            ) < 1e-9
        );
    }
}

void testInvalidRootCandidateDoesNotBecomeBestMove() {
    BoardState board = makeBoard();

    TurnPlan invalidPlan;

    // Non-adjacent destination: movement validator must reject it.
    invalidPlan.moves.push_back({
        {1, 1},
        4,
        {3, 3}
    });

    const std::vector<TurnPlan> candidates{
        invalidPlan
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            makeTestOptions()
        );

    assert(!result.valid());
    assert(!result.hasBestCandidate);
}

} // namespace

int main() {
    testInvalidBoard();
    testInvalidSpecies();
    testEmptyCandidates();
    testInvalidOptions();
    testSearchReturnsOriginalCandidateIndex();
    testOpponentResponseBudgetIsRespected();
    testRootCandidateBudgetIsRespected();
    testNoOpponentResponseCanBeEvaluated();
    testOriginalBoardIsNotModified();
    testSearchIsReproducible();
    testInvalidRootCandidateDoesNotBecomeBestMove();

    std::cout << "All opponent search tests passed.\n";
    return 0;
}
