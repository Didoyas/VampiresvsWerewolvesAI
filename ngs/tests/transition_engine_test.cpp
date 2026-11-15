
#include "../src/transition_engine.hpp"

#include <cassert>
#include <iostream>
#include <random>

using namespace vvw::ngs;

namespace {

BoardState createTestBoard() {
    BoardState board;

    board.rows = 5;
    board.columns = 5;
    board.cells.resize(25);

    board.ourSpecies = Species::Vampires;

    // The previous-ply footprint is available and empty.
    board.previousPly.known = true;

    return board;
}

TurnPlan createPlan(
    Position source,
    Population count,
    Position destination
) {
    TurnPlan plan;
    plan.moves.push_back({source, count, destination});
    return plan;
}

// -----------------------------------------------------------------------------
// Probability tests
// -----------------------------------------------------------------------------

void testEqualStrengthProbability() {
    assert(attackerWinProbability(10, 10) == 0.5);
}

void testWeakerAttackerProbability() {
    assert(attackerWinProbability(4, 8) == 0.25);
}

void testStrongerAttackerProbability() {
    assert(attackerWinProbability(10, 8) == 0.75);
}

// -----------------------------------------------------------------------------
// Basic movement tests
// -----------------------------------------------------------------------------

void testMoveIntoEmptyCell() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 10;

    TurnPlan plan = createPlan(
        {1, 1},
        5,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(result.valid());

    assert(result.nextState.at({1, 1}).vampires == 5);
    assert(result.nextState.at({2, 1}).vampires == 5);

    // The original board must remain unchanged.
    assert(board.at({1, 1}).vampires == 10);
    assert(board.at({2, 1}).vampires == 0);
}

void testFriendlyGroupMerge() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 10;
    board.at({2, 1}).vampires = 7;

    TurnPlan plan = createPlan(
        {1, 1},
        8,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(result.valid());

    assert(result.nextState.at({1, 1}).vampires == 2);
    assert(result.nextState.at({2, 1}).vampires == 15);
}

// -----------------------------------------------------------------------------
// Human conversion tests
// -----------------------------------------------------------------------------

void testDirectHumanConversion() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 10;
    board.at({2, 1}).humans = 5;

    // Five attackers against five humans.
    // This meets the normal conversion threshold.
    TurnPlan plan = createPlan(
        {1, 1},
        5,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(result.valid());

    assert(result.nextState.at({1, 1}).vampires == 5);
    assert(result.nextState.at({2, 1}).vampires == 10);
    assert(result.nextState.at({2, 1}).humans == 0);

    assert(result.stats.humansConverted == 5);
}

void testHumanRandomBattleIsReproducible() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 4;
    board.at({2, 1}).humans = 10;

    TurnPlan plan = createPlan(
        {1, 1},
        4,
        {2, 1}
    );

    // Same initial seed means same sampled battle outcome.
    std::mt19937 rng1(12345);
    std::mt19937 rng2(12345);

    const TransitionResult first =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng1
        );

    const TransitionResult second =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng2
        );

    assert(first.valid());
    assert(second.valid());

    assert(first.stats.randomBattles == 1);
    assert(second.stats.randomBattles == 1);

    assert(
        first.nextState.at({2, 1}).vampires ==
        second.nextState.at({2, 1}).vampires
    );

    assert(
        first.nextState.at({2, 1}).humans ==
        second.nextState.at({2, 1}).humans
    );
}

// -----------------------------------------------------------------------------
// Deterministic creature combat
// -----------------------------------------------------------------------------

void testAttackerWinsAtExactOneAndHalfThreshold() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 9;
    board.at({2, 1}).werewolves = 6;

    // 9 / 6 = 1.5.
    TurnPlan plan = createPlan(
        {1, 1},
        9,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(result.valid());

    assert(result.nextState.at({2, 1}).vampires == 9);
    assert(result.nextState.at({2, 1}).werewolves == 0);

    assert(result.stats.deterministicCreatureWins == 1);
}

void testDefenderWinsAtExactOneAndHalfThreshold() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 6;
    board.at({2, 1}).werewolves = 9;

    // Defenders are exactly 1.5 times the attackers.
    TurnPlan plan = createPlan(
        {1, 1},
        6,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(result.valid());

    assert(result.nextState.at({1, 1}).vampires == 0);
    assert(result.nextState.at({2, 1}).vampires == 0);
    assert(result.nextState.at({2, 1}).werewolves == 9);

    assert(result.stats.deterministicCreatureLosses == 1);
}

// -----------------------------------------------------------------------------
// Random creature combat
// -----------------------------------------------------------------------------

void testRandomCreatureBattleIsReproducible() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 8;
    board.at({2, 1}).werewolves = 10;

    // Neither side reaches the 1.5 threshold.
    TurnPlan plan = createPlan(
        {1, 1},
        8,
        {2, 1}
    );

    std::mt19937 rng1(9876);
    std::mt19937 rng2(9876);

    const TransitionResult first =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng1
        );

    const TransitionResult second =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng2
        );

    assert(first.valid());
    assert(second.valid());

    assert(first.stats.randomBattles == 1);
    assert(second.stats.randomBattles == 1);

    const Cell& firstCell = first.nextState.at({2, 1});
    const Cell& secondCell = second.nextState.at({2, 1});

    assert(firstCell.vampires == secondCell.vampires);
    assert(firstCell.werewolves == secondCell.werewolves);

    // Only one side can remain after a creature battle.
    assert(
        firstCell.vampires == 0 ||
        firstCell.werewolves == 0
    );
}

// -----------------------------------------------------------------------------
// Multiple movements
// -----------------------------------------------------------------------------

void testMultipleGroupsArrivingAtSameDestination() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 5;
    board.at({3, 1}).vampires = 5;

    board.at({2, 1}).werewolves = 6;

    TurnPlan plan;

    plan.moves.push_back({{1, 1}, 5, {2, 1}});
    plan.moves.push_back({{3, 1}, 5, {2, 1}});

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(result.valid());

    // The engine aggregates the two arrivals:
    // 10 attackers against 6 defenders.
    // 10 >= 1.5 * 6, so the attackers win deterministically.
    assert(result.nextState.at({2, 1}).vampires == 10);
    assert(result.nextState.at({2, 1}).werewolves == 0);

    assert(result.stats.movesApplied == 2);
}

// -----------------------------------------------------------------------------
// Previous-ply footprint
// -----------------------------------------------------------------------------

void testCurrentPlyFootprintIsRecorded() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 10;

    TurnPlan plan = createPlan(
        {1, 1},
        5,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(result.valid());

    const PlyFootprint& footprint =
        result.nextState.previousPly;

    assert(footprint.known);
    assert(footprint.sources.size() == 1);
    assert(footprint.destinations.size() == 1);

    assert(footprint.wasSource({1, 1}));
    assert(footprint.wasDestination({2, 1}));
}

// -----------------------------------------------------------------------------
// Validation failures
// -----------------------------------------------------------------------------

void testInvalidPlanIsRejected() {
    BoardState board = createTestBoard();

    // The source contains no vampires.
    TurnPlan plan = createPlan(
        {1, 1},
        5,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(!result.valid());
    assert(result.error == TransitionError::InvalidPlan);

    assert(
        result.validationError ==
        ValidationError::SourceDoesNotContainOurSpecies
    );

    // Original board is still unchanged.
    assert(board.at({1, 1}).vampires == 0);
}

void testUnknownPreviousFootprintIsRejectedByDefault() {
    BoardState board = createTestBoard();

    board.previousPly.known = false;
    board.at({1, 1}).vampires = 10;

    TurnPlan plan = createPlan(
        {1, 1},
        5,
        {2, 1}
    );

    std::mt19937 rng(42);

    const TransitionResult result =
        applyTurnPlan(
            board,
            plan,
            Species::Vampires,
            rng
        );

    assert(!result.valid());

    assert(
        result.validationError ==
        ValidationError::MissingPreviousPlyFootprint
    );
}

} // namespace

int main() {
    testEqualStrengthProbability();
    testWeakerAttackerProbability();
    testStrongerAttackerProbability();

    testMoveIntoEmptyCell();
    testFriendlyGroupMerge();

    testDirectHumanConversion();
    testHumanRandomBattleIsReproducible();

    testAttackerWinsAtExactOneAndHalfThreshold();
    testDefenderWinsAtExactOneAndHalfThreshold();
    testRandomCreatureBattleIsReproducible();

    testMultipleGroupsArrivingAtSameDestination();

    testCurrentPlyFootprintIsRecorded();

    testInvalidPlanIsRejected();
    testUnknownPreviousFootprintIsRejectedByDefault();

    std::cout << "All transition engine tests passed.\n";

    return 0;
}
