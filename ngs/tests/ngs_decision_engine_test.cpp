
#include "ngs_decision_engine.hpp"
#include "movement_rules.hpp"
#include "vvw/game/decision_engine_factory.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

    // Validate using the same-ply movement rules. The assignment does not
    // impose a previous-ply movement-footprint restriction.
    vvw::ngs::ValidationOptions options;

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

void testNoLegalMoveReportsSpecificError() {
    vvw::game::BoardState board = makeBoard();

    // Remove all our vampires. The board remains structurally valid,
    // but there is no friendly group capable of making a move.
    board.at({1, 1}).vampires = 0;
    board.at({2, 1}).humans = 0;

    vvw::ngs::NgsDecisionEngine engine(
        vvw::game::Species::Vampires
    );

    bool threwExpectedError = false;

    try {
        (void)engine.chooseMove(board);
    } catch (const std::runtime_error& error) {
        threwExpectedError = true;
        assert(
            std::string(error.what()).find(
                "NGS agent found no legal move"
            ) != std::string::npos
        );
    }

    assert(threwExpectedError);
}

void testInvalidInputReportsSpecificError() {
    vvw::game::BoardState board = makeBoard();

    // The adapter controls Vampires, but the board claims that
    // Werewolves are the player species.
    board.ourSpecies = vvw::game::Species::Werewolves;

    vvw::ngs::NgsDecisionEngine engine(
        vvw::game::Species::Vampires
    );

    bool threwExpectedError = false;

    try {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds{100};

        (void)engine.chooseMove(board, deadline);
    } catch (const std::runtime_error& error) {
        threwExpectedError = true;
        assert(
            std::string(error.what()).find(
                "NGS agent rejected the board"
            ) != std::string::npos
        );
    }

    assert(threwExpectedError);
}

} // namespace

int main() {
    testNgsFactoryAndDecisionEngineContract();
    testNoLegalMoveReportsSpecificError();
    testInvalidInputReportsSpecificError();

    std::cout << "NGS DecisionEngine adapter tests passed.\n";
    return 0;
}
