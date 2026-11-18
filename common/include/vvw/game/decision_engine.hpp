#pragma once

#include "vvw/game/types.hpp"

#include <chrono>

namespace vvw::game {

// Absolute deadline measured against std::chrono::steady_clock. The client
// creates it once per UPD and passes the same value through the engine stack.
using DecisionDeadline = std::chrono::steady_clock::time_point;

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

    // Deadline-aware entry point. The default preserves compatibility with
    // engines that have not yet implemented cooperative deadline handling.
    // Such engines still run their legacy chooseMove() implementation and do
    // NOT gain a deadline guarantee until they override this overload.
    virtual TurnPlan chooseMove(
        const BoardState& board,
        DecisionDeadline deadline
    ) {
        (void)deadline;
        return chooseMove(board);
    }
};

} // namespace vvw::game