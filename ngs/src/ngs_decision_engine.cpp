#include "ngs_decision_engine.hpp"

namespace vvw::ngs {

NgsDecisionEngine::NgsDecisionEngine(
    vvw::game::Species ourSpecies
)
    : agent_(ourSpecies) {
}

bool NgsDecisionEngine::onGameStart(
    const vvw::game::BoardState& initialBoard
) {
    return agent_.observeInitialState(initialBoard);
}

vvw::game::TurnPlan NgsDecisionEngine::chooseMove(
    const vvw::game::BoardState& board
) {
    const AgentDecision decision = agent_.chooseMove(board);

    return decision.plan;
}

vvw::game::TurnPlan NgsDecisionEngine::chooseMove(
    const vvw::game::BoardState& board,
    vvw::game::DecisionDeadline deadline
) {
    const AgentDecision decision = agent_.chooseMove(board, deadline);

    return decision.plan;
}

} // namespace vvw::ngs