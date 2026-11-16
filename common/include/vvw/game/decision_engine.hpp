#pragma once

#include "vvw/game/types.hpp"

namespace vvw::game {

class DecisionEngine {
public:
    virtual ~DecisionEngine() = default;

    // Called once after SET/HUM/HME/MAP for each game.
    virtual bool onGameStart(const BoardState& initialBoard) {
        (void)initialBoard;
        return true;
    }

    // Called for every UPD. Returns a complete turn plan.
    virtual TurnPlan chooseMove(const BoardState& board) = 0;
};

} // namespace vvw::game