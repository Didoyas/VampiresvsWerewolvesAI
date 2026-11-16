#include "vvw/game/decision_engine.hpp"

#include <cassert>
#include <iostream>
#include <memory>

namespace {

class TestDecisionEngine final : public vvw::game::DecisionEngine {
public:
    vvw::game::TurnPlan chooseMove(
        const vvw::game::BoardState& board
    ) override {
        vvw::game::TurnPlan plan;
        if (!board.hasValidStorage()) {
            return plan;
        }

        plan.moves.push_back({
            vvw::game::Position{0, 0},
            1,
            vvw::game::Position{1, 0}
        });
        return plan;
    }
};

void testInterfaceSupportsPolymorphicEngineUse() {
    vvw::game::BoardState board;
    board.rows = 2;
    board.columns = 2;
    board.cells.resize(4);
    board.ourSpecies = vvw::game::Species::Vampires;

    std::unique_ptr<vvw::game::DecisionEngine> engine =
        std::make_unique<TestDecisionEngine>();

    // The interface's default game-start hook is suitable for stateless
    // algorithms; stateful algorithms can override it.
    assert(engine->onGameStart(board));

    const vvw::game::TurnPlan plan = engine->chooseMove(board);
    assert(plan.moves.size() == 1);
    assert((plan.moves.front().source == vvw::game::Position{0, 0}));
    assert((plan.moves.front().destination == vvw::game::Position{1, 0}));
    assert(plan.moves.front().count == 1);
}

} // namespace

int main() {
    testInterfaceSupportsPolymorphicEngineUse();
    std::cout << "DecisionEngine contract test passed.\n";
    return 0;
}
