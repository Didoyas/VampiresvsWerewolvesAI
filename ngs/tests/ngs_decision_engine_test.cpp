#include "ngs_decision_engine.hpp"
#include "movement_rules.hpp"
#include "vvw/game/decision_engine_factory.hpp"

#include <cassert>
#include <iostream>
#include <memory>

namespace {

vvw::game::BoardState makeBoard() {
    vvw::game::BoardState board;
    board.rows = 6;
    board.columns = 6;
    board.cells.resize(36);
    board.ourSpecies = vvw::game::Species::Vampires;

    // Our group has a guaranteed legal adjacent human target.
    board.at({1, 1}).vampires = 10;
    board.at({2, 1}).humans = 4;

    // Add an opponent group and a house to make this a representative board.
    board.at({4, 4}).werewolves = 4;
    board.at({4, 3}).humans = 3;
    board.at({4, 3}).isHouse = true;

    // UPD does not disclose the opponent's exact previous MOV footprint.
    board.previousPly.known = false;
    return board;
}

void testNgsFactoryAndDecisionEngineContract() {
    const vvw::game::BoardState board = makeBoard();

    std::unique_ptr<vvw::game::DecisionEngine> engine =
        vvw::game::makeDecisionEngine(vvw::game::Species::Vampires);

    assert(engine != nullptr);
    assert(engine->onGameStart(board));

    const vvw::game::TurnPlan plan = engine->chooseMove(board);
    assert(!plan.moves.empty());
    assert(plan.moves.size() <= 255);

    // Validate using NGS's movement validator, while explicitly disabling
    // the cross-ply check because the server protocol does not provide the
    // previous opponent move footprint.
    vvw::ngs::ValidationOptions options;
    options.requirePreviousPlyFootprint = false;

    const vvw::ngs::ValidationResult validation =
        vvw::ngs::validateTurnPlan(
            board,
            plan,
            vvw::game::Species::Vampires,
            options
        );

    assert(validation.valid());

    for (const vvw::game::Move& move : plan.moves) {
        assert(move.count > 0);
        assert(move.count <= 255);
        assert(board.inBounds(move.source));
        assert(board.inBounds(move.destination));
    }
}

} // namespace

int main() {
    testNgsFactoryAndDecisionEngineContract();
    std::cout << "NGS DecisionEngine adapter test passed.\n";
    return 0;
}
