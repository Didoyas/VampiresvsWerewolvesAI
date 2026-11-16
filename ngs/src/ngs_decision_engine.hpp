#pragma once

#include "agent.hpp"
#include "vvw/game/decision_engine.hpp"

namespace vvw::ngs {

class NgsDecisionEngine final : public vvw::game::DecisionEngine {
public:
    explicit NgsDecisionEngine(vvw::game::Species ourSpecies);

    bool onGameStart(
        const vvw::game::BoardState& initialBoard
    ) override;

    vvw::game::TurnPlan chooseMove(
        const vvw::game::BoardState& board
    ) override;

private:
    Agent agent_;
};

} // namespace vvw::ngs