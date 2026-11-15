#include "agent.hpp"
#include "movement_rules.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace vvw::ngs;

namespace {

BoardState makeBoard() {
    BoardState board;
    board.rows = 6;
    board.columns = 6;
    board.cells.resize(36);
    board.ourSpecies = Species::Vampires;

    board.at({1, 1}).vampires = 10;
    board.at({2, 1}).humans = 4;
    board.at({4, 4}).werewolves = 4;
    board.at({4, 3}).humans = 3;
    board.at({4, 3}).isHouse = true;

    // The current TCP UPD protocol does not identify the opponent's prior MOV.
    board.previousPly.known = false;
    return board;
}

void testAgentReturnsAValidatedPlanAndRecordsHistory() {
    BoardState board = makeBoard();
    const BoardState original = board;

    AgentOptions options;
    options.searchBudget = std::chrono::milliseconds{0};
    Agent agent(Species::Vampires, options);

    const AgentDecision decision = agent.chooseMove(board);

    assert(decision.hasMove());
    assert(decision.turnNumber == 1);
    assert(decision.previousPlyWasUnknown);
    assert(!decision.crossPlyRestrictionsChecked);
    assert(decision.historyUpdated);
    assert(decision.method == AgentDecisionMethod::CandidateFallback ||
           decision.method == AgentDecisionMethod::EmergencyFallback);

    ValidationOptions validationOptions;
    validationOptions.requirePreviousPlyFootprint = false;
    assert(validateTurnPlan(
        board,
        decision.plan,
        Species::Vampires,
        validationOptions
    ).valid());

    assert(agent.history().size() == 1);
    assert(agent.nextTurnNumber() == 2);

    // chooseMove accepts a const state and must not mutate the protocol snapshot.
    assert(board.rows == original.rows);
    assert(board.columns == original.columns);
    assert(board.cells.size() == original.cells.size());
    for (std::size_t i = 0; i < board.cells.size(); ++i) {
        assert(board.cells[i].humans == original.cells[i].humans);
        assert(board.cells[i].vampires == original.cells[i].vampires);
        assert(board.cells[i].werewolves == original.cells[i].werewolves);
        assert(board.cells[i].isHouse == original.cells[i].isHouse);
    }
}

void testHistoryAdvancesAcrossUpdates() {
    BoardState board = makeBoard();
    AgentOptions options;
    options.searchBudget = std::chrono::milliseconds{0};
    Agent agent(Species::Vampires, options);

    const AgentDecision first = agent.chooseMove(board);
    assert(first.hasMove());

    // Model the next server UPD snapshot. No movement event is fabricated.
    board.at({4, 4}).werewolves = 0;
    board.at({3, 4}).werewolves = 4;

    const AgentDecision second = agent.chooseMove(board);
    assert(second.hasMove());
    assert(second.turnNumber == 2);
    assert(second.historyUpdated);
    assert(agent.history().size() == 2);
    assert(agent.nextTurnNumber() == 3);
}

void testNoFriendlyGroupsReturnsNoLegalMove() {
    BoardState board;
    board.rows = 3;
    board.columns = 3;
    board.cells.resize(9);
    board.ourSpecies = Species::Vampires;
    board.at({1, 1}).werewolves = 5;
    board.previousPly.known = false;

    AgentOptions options;
    options.searchBudget = std::chrono::milliseconds{0};
    Agent agent(Species::Vampires, options);

    const AgentDecision decision = agent.chooseMove(board);

    assert(!decision.hasMove());
    assert(decision.method == AgentDecisionMethod::NoLegalMove);
}

void testInvalidBoardDoesNotRecordHistory() {
    BoardState board = makeBoard();
    board.cells.pop_back();

    Agent agent(Species::Vampires);
    const AgentDecision decision = agent.chooseMove(board);

    assert(!decision.hasMove());
    assert(decision.method == AgentDecisionMethod::InvalidInput);
    assert(agent.history().empty());
}

} // namespace

int main() {
    testAgentReturnsAValidatedPlanAndRecordsHistory();
    testHistoryAdvancesAcrossUpdates();
    testNoFriendlyGroupsReturnsNoLegalMove();
    testInvalidBoardDoesNotRecordHistory();

    std::cout << "All agent orchestration tests passed.\n";
    return 0;
}
