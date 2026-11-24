
#include "../src/movement_rules.hpp"

#include <cassert>
#include <iostream>

using namespace vvw::ngs;

namespace {

BoardState createTestBoard() {
    BoardState board;

    board.rows = 5;
    board.columns = 5;
    board.cells.resize(25);

    board.ourSpecies = Species::Vampires;

    // Populate the legacy footprint metadata; it must not affect legality.
    board.previousPly.known = true;

    // Our vampire groups.
    board.at({1, 1}).vampires = 20;
    board.at({2, 2}).vampires = 10;

    // Opponent's werewolf groups.
    board.at({3, 3}).werewolves = 15;

    // Human population.
    board.at({1, 2}).humans = 8;

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

void testValidHorizontalMovement() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 10, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testValidVerticalMovement() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 10, {1, 2});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testValidDiagonalMovement() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 10, {2, 2});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testMovementOutsideBoard() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 10, {5, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error == ValidationError::PositionOutOfBounds);
}

void testNonAdjacentMovement() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 10, {3, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error == ValidationError::NotAdjacent);
}

void testMovementToSameCell() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 10, {1, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error == ValidationError::NotAdjacent);
}

void testUnknownSpecies() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 10, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Unknown);

    assert(result.error == ValidationError::UnknownSpecies);
}

void testEmptyPlan() {
    BoardState board = createTestBoard();

    TurnPlan plan;

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error == ValidationError::EmptyPlan);
}

void testZeroMovement() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({1, 1}, 0, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error == ValidationError::ZeroMovement);
}

void testMovementExceedsWireLimit() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 300;

    TurnPlan plan = createPlan({1, 1}, 256, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error == ValidationError::MovementExceedsWireLimit);
}

void testMaximumWireMovementAllowed() {
    BoardState board = createTestBoard();

    board.at({1, 1}).vampires = 300;

    TurnPlan plan = createPlan({1, 1}, 255, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testWrongSpeciesAtSource() {
    BoardState board = createTestBoard();

    // This cell contains werewolves, not vampires.
    TurnPlan plan = createPlan({3, 3}, 10, {3, 2});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::SourceDoesNotContainOurSpecies);
}

void testOpponentSpeciesMovement() {
    BoardState board = createTestBoard();

    TurnPlan plan = createPlan({3, 3}, 10, {3, 2});

    const auto result =
        validateTurnPlan(board, plan, Species::Werewolves);

    assert(result.valid());
}

void testInsufficientPopulation() {
    BoardState board = createTestBoard();

    // The source has only 20 vampires.
    TurnPlan plan = createPlan({1, 1}, 21, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::InsufficientSourcePopulation);
}

void testMultipleSplitsWithinPopulation() {
    BoardState board = createTestBoard();

    TurnPlan plan;
    plan.moves.push_back({{1, 1}, 10, {2, 1}});
    plan.moves.push_back({{1, 1}, 10, {2, 2}});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testMultipleSplitsExceedPopulation() {
    BoardState board = createTestBoard();

    // 12 + 10 = 22, but the source contains only 20.
    TurnPlan plan;
    plan.moves.push_back({{1, 1}, 12, {2, 1}});
    plan.moves.push_back({{1, 1}, 10, {2, 2}});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::InsufficientSourcePopulation);
}

void testPreviousPlyFootprintDoesNotRestrictMovement() {
    BoardState board = createTestBoard();
    board.previousPly.known = true;
    board.previousPly.destinations.push_back({1, 1});
    board.previousPly.sources.push_back({2, 1});

    // Both movements would have been blocked by the old cross-ply rule.
    // The assignment's same-ply constraints remain enforced separately.
    const TurnPlan fromPreviousDestination =
        createPlan({1, 1}, 10, {2, 1});
    const TurnPlan intoPreviousSource =
        createPlan({1, 1}, 10, {2, 1});

    assert(validateTurnPlan(
        board, fromPreviousDestination, Species::Vampires).valid());
    assert(validateTurnPlan(
        board, intoPreviousSource, Species::Vampires).valid());

    board.previousPly.known = false;
    assert(validateTurnPlan(
        board, fromPreviousDestination, Species::Vampires).valid());
}

void testSamePlySourceDestinationConflict() {
    BoardState board = createTestBoard();

    board.at({2, 1}).vampires = 10;

    TurnPlan plan;

    // Cell (2,1) is both a destination and a source.
    plan.moves.push_back({{1, 1}, 10, {2, 1}});
    plan.moves.push_back({{2, 1}, 10, {2, 2}});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::SamePlySourceDestinationConflict);
}

void testInvalidBoardStorage() {
    BoardState board = createTestBoard();

    // Deliberately make storage inconsistent with dimensions.
    board.cells.pop_back();

    TurnPlan plan = createPlan({1, 1}, 10, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error == ValidationError::InvalidBoard);
}


void testDestinationPopulationAtWireLimitAllowed() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 255;

    TurnPlan plan = createPlan({1, 1}, 255, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testMultipleArrivalsAtDestinationWithinWireLimit() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 200;
    board.at({2, 2}).vampires = 55;

    TurnPlan plan;
    plan.moves.push_back({{1, 1}, 200, {2, 1}});
    plan.moves.push_back({{2, 2}, 55, {2, 1}});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testMultipleArrivalsExceedDestinationWireLimit() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 200;
    board.at({2, 2}).vampires = 56;

    TurnPlan plan;
    plan.moves.push_back({{1, 1}, 200, {2, 1}});
    plan.moves.push_back({{2, 2}, 56, {2, 1}});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::DestinationPopulationExceedsWireLimit);
    assert(result.moveIndex == 0);
}

void testFriendlyDestinationMergeAtWireLimitAllowed() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 245;
    board.at({2, 1}).vampires = 10;

    TurnPlan plan = createPlan({1, 1}, 245, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testFriendlyDestinationMergeExceedsWireLimit() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 246;
    board.at({2, 1}).vampires = 10;

    TurnPlan plan = createPlan({1, 1}, 246, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::DestinationPopulationExceedsWireLimit);
}

void testDeterministicHumanConversionAtWireLimitAllowed() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 250;
    board.at({2, 1}).humans = 5;

    TurnPlan plan = createPlan({1, 1}, 250, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.valid());
}

void testDeterministicHumanConversionExceedsWireLimit() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 250;
    board.at({2, 1}).humans = 10;

    TurnPlan plan = createPlan({1, 1}, 250, {2, 1});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::DestinationPopulationExceedsWireLimit);
}

void testRandomHumanConversionOverflowDependsOnOption() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 100;
    board.at({2, 1}).humans = 200;

    TurnPlan plan = createPlan({1, 1}, 100, {2, 1});

    // When post-random-battle conversion is enabled, an attacker victory
    // could produce a friendly population above the one-byte wire limit.
    ValidationOptions conversionEnabled;
    const auto enabledResult = validateTurnPlan(
        board, plan, Species::Vampires, conversionEnabled);

    assert(enabledResult.error ==
           ValidationError::DestinationPopulationExceedsWireLimit);

    // When conversion is disabled, surviving humans remain human. The
    // friendly population cannot exceed the 100 arriving vampires.
    ValidationOptions conversionDisabled;
    conversionDisabled.convertHumansAfterRandomBattle = false;
    const auto disabledResult = validateTurnPlan(
        board, plan, Species::Vampires, conversionDisabled);

    assert(disabledResult.valid());
}

void testEnemyEncounterWithPossibleAttackerWinExceedsWireLimit() {
    BoardState board = createTestBoard();
    board.at({1, 1}).vampires = 200;
    board.at({2, 2}).vampires = 56;
    board.at({2, 1}).werewolves = 255;

    TurnPlan plan;
    plan.moves.push_back({{1, 1}, 200, {2, 1}});
    plan.moves.push_back({{2, 2}, 56, {2, 1}});

    const auto result =
        validateTurnPlan(board, plan, Species::Vampires);

    assert(result.error ==
           ValidationError::DestinationPopulationExceedsWireLimit);
}

} // namespace

int main() {
    testValidHorizontalMovement();
    testValidVerticalMovement();
    testValidDiagonalMovement();

    testMovementOutsideBoard();
    testNonAdjacentMovement();
    testMovementToSameCell();

    testUnknownSpecies();
    testEmptyPlan();
    testZeroMovement();

    testMovementExceedsWireLimit();
    testMaximumWireMovementAllowed();

    testWrongSpeciesAtSource();
    testOpponentSpeciesMovement();

    testInsufficientPopulation();
    testMultipleSplitsWithinPopulation();
    testMultipleSplitsExceedPopulation();

    testPreviousPlyFootprintDoesNotRestrictMovement();

    testSamePlySourceDestinationConflict();
    testInvalidBoardStorage();

    testDestinationPopulationAtWireLimitAllowed();
    testMultipleArrivalsAtDestinationWithinWireLimit();
    testMultipleArrivalsExceedDestinationWireLimit();
    testFriendlyDestinationMergeAtWireLimitAllowed();
    testFriendlyDestinationMergeExceedsWireLimit();
    testDeterministicHumanConversionAtWireLimitAllowed();
    testDeterministicHumanConversionExceedsWireLimit();
    testRandomHumanConversionOverflowDependsOnOption();
    testEnemyEncounterWithPossibleAttackerWinExceedsWireLimit();

    std::cout << "All movement rules tests passed.\n";

    return 0;
}
