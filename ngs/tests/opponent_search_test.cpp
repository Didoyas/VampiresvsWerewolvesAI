
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


BoardState makeStage7Board() {
    BoardState board;

    board.rows = 6;
    board.columns = 6;
    board.cells.resize(36);

    board.ourSpecies = Species::Vampires;

    // Our group.
    board.at({1, 1}).vampires = 4;

    // Humans that we can convert.
    board.at({2, 1}).humans = 2;

    // Opponent group.
    // Positioned adjacent to our post-move destination.
    board.at({3, 2}).werewolves = 9;

    // Human house towards which the opponent has moved.
    board.at({5, 3}).humans = 4;
    board.at({5, 3}).isHouse = true;

    board.previousPly.known = true;

    return board;
}

TurnPlan makeStage7RootPlan() {
    TurnPlan plan;

    plan.moves.push_back({
        {1, 1},
        4,
        {2, 1}
    });

    return plan;
}

OpponentHistory makeStage7History() {
    OpponentHistory history(Species::Werewolves);

    // Turn 1: opponent group at (1,2).
    BoardState turn1 = makeStage7Board();

    turn1.at({3, 2}).werewolves = 0;
    turn1.at({1, 2}).werewolves = 9;

    assert(history.recordObservation(turn1, 1).valid());

    assert(
        history.recordConfirmedMovement(
            1, {0, 2}, {1, 2}, 9
        ) == OpponentHistoryError::None
    );

    // Turn 2: opponent group moves east.
    BoardState turn2 = makeStage7Board();

    turn2.at({3, 2}).werewolves = 0;
    turn2.at({2, 2}).werewolves = 9;

    assert(history.recordObservation(turn2, 2).valid());

    assert(
        history.recordConfirmedMovement(
            2, {1, 2}, {2, 2}, 9
        ) == OpponentHistoryError::None
    );

    // Turn 3: opponent group reaches (3,2).
    BoardState turn3 = makeStage7Board();

    assert(history.recordObservation(turn3, 3).valid());

    assert(
        history.recordConfirmedMovement(
            3, {2, 2}, {3, 2}, 9
        ) == OpponentHistoryError::None
    );

    return history;
}

OpponentSearchOptions makeStage7Options() {
    OpponentSearchOptions options;

    options.maxRootCandidates = 1;

    options.maxOpponentResponses = 8;
    options.maxLikelyOpponentResponses = 4;
    options.maxAdversarialOpponentResponses = 8;

    options.simulationsPerRootCandidate = 1;

    options.expectedResponseWeight = 0.70;
    options.worstCaseResponseWeight = 0.30;

    options.rootRankingOptions.simulationsPerCandidate = 1;
    options.rootRankingOptions.maxCandidatesToEvaluate = 8;

    options.opponentGenerationOptions.maxSingleMoveCandidates = 32;
    options.opponentGenerationOptions.maxTwoMoveCandidates = 16;
    options.opponentGenerationOptions.maxThreeMoveCandidates = 8;
    options.opponentGenerationOptions.maxCombinationAttempts = 2000;

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


void testStage7UsesHistoricalPredictions() {
    BoardState board = makeStage7Board();

    OpponentHistory history = makeStage7History();

    const std::vector<TurnPlan> candidates{
        makeStage7RootPlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            history,
            3,
            makeStage7Options()
        );

    assert(result.valid());
    assert(result.hasBestCandidate);

    assert(result.stats.predictionsUsed);
    assert(!result.stats.predictionFallbackUsed);

    assert(result.stats.responsesSelectedByPrediction > 0);

    assert(result.stats.opponentResponsesExamined > 0);

    assert(result.bestCandidateIndex == 0);
}

void testStage7AlsoSelectsAdversarialResponses() {
    BoardState board = makeStage7Board();

    OpponentHistory history = makeStage7History();

    const std::vector<TurnPlan> candidates{
        makeStage7RootPlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            history,
            3,
            makeStage7Options()
        );

    assert(result.valid());

    // Predictions must not be the only selection mechanism.
    assert(
        result.stats.responsesSelectedForAdversarialCoverage > 0
    );

    assert(result.stats.opponentResponsesEvaluated > 0);
}

void testStage7FallsBackWhenHistoryIsUnavailable() {
    BoardState board = makeStage7Board();

    // Default history has no configured opponent species
    // and contains no observations.
    OpponentHistory history;

    const std::vector<TurnPlan> candidates{
        makeStage7RootPlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            history,
            1,
            makeStage7Options()
        );

    // Search should still work using tactical response selection.
    assert(result.valid());

    assert(!result.stats.predictionsUsed);
    assert(result.stats.predictionFallbackUsed);

    assert(
        result.predictionError ==
        OpponentPredictionError::InvalidSpecies
    );

    assert(result.hasBestCandidate);
}

void testStage7PreservesOriginalSearchAPI() {
    BoardState board = makeStage7Board();

    const std::vector<TurnPlan> candidates{
        makeStage7RootPlan()
    };

    // The original history-independent API must continue to work.
    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            makeStage7Options()
        );

    assert(result.valid());
    assert(result.hasBestCandidate);

    assert(!result.stats.predictionsUsed);
    assert(!result.stats.predictionFallbackUsed);
}

void testStage7FallsBackToLegalResponsesWithoutPredictionOrThreat() {
    BoardState board = makeBoard();

    // The root move converts the only humans. The opponent group
    // has legal moves, but none directly attacks our group or
    // converts humans on this board. With no prediction supplied,
    // response selection must still retain legal alternatives.
    const std::vector<TurnPlan> candidates{
        makeConversionPlan()
    };

    OpponentSearchOptions options = makeTestOptions();
    options.maxRootCandidates = 1;
    options.maxOpponentResponses = 4;
    options.maxLikelyOpponentResponses = 4;
    options.maxAdversarialOpponentResponses = 4;

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            options
        );

    assert(result.valid());
    assert(result.hasBestCandidate);
    assert(result.stats.opponentResponsesGenerated > 0);
    assert(result.stats.opponentResponsesExamined > 0);
    assert(result.stats.opponentResponsesEvaluated > 0);
    assert(result.stats.responsesSelectedByPrediction == 0);
    assert(result.stats.responsesSelectedForAdversarialCoverage == 0);
}

void testStage7RejectsActingSpeciesMismatch() {
    BoardState board = makeStage7Board();

    const std::vector<TurnPlan> candidates{
        makeStage7RootPlan()
    };

    // BoardState says Vampires are ours, so Werewolves cannot be
    // passed as the acting species for this search.
    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Werewolves,
            makeStage7Options()
        );

    assert(!result.valid());
    assert(result.error == OpponentSearchError::InvalidSpecies);
}

void testStage7RejectsInvalidResponseWeights() {
    BoardState board = makeStage7Board();

    const std::vector<TurnPlan> candidates{
        makeStage7RootPlan()
    };

    OpponentSearchOptions options = makeStage7Options();

    options.expectedResponseWeight = 0.0;
    options.worstCaseResponseWeight = 0.0;

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

void testStage7DoesNotModifyBoardOrHistory() {
    BoardState board = makeStage7Board();

    const Population originalVampires =
        board.at({1, 1}).vampires;

    const Population originalWerewolves =
        board.at({3, 2}).werewolves;

    const Population originalHumans =
        board.at({2, 1}).humans;

    OpponentHistory history = makeStage7History();

    const std::size_t originalSnapshotCount =
        history.size();

    const std::size_t originalMovementCount =
        history.confirmedMovements().size();

    const std::vector<TurnPlan> candidates{
        makeStage7RootPlan()
    };

    const OpponentSearchResult result =
        searchTurnCandidates(
            board,
            candidates,
            Species::Vampires,
            history,
            3,
            makeStage7Options()
        );

    assert(result.valid());

    assert(board.at({1, 1}).vampires == originalVampires);
    assert(board.at({3, 2}).werewolves == originalWerewolves);
    assert(board.at({2, 1}).humans == originalHumans);

    assert(history.size() == originalSnapshotCount);

    assert(
        history.confirmedMovements().size() ==
        originalMovementCount
    );
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

    // Stage 7: history-aware opponent search.
    testStage7UsesHistoricalPredictions();
    testStage7AlsoSelectsAdversarialResponses();
    testStage7FallsBackWhenHistoryIsUnavailable();
    testStage7PreservesOriginalSearchAPI();
    testStage7FallsBackToLegalResponsesWithoutPredictionOrThreat();
    testStage7RejectsActingSpeciesMismatch();
    testStage7RejectsInvalidResponseWeights();
    testStage7DoesNotModifyBoardOrHistory();

    std::cout << "All opponent search tests passed.\n";
    return 0;
}
