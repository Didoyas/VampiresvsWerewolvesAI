#include "ngs_decision_engine.hpp"

#include <stdexcept>

namespace vvw::ngs {
namespace {

vvw::game::TurnPlan requireDecisionPlan(
    const AgentDecision& decision
) {
    if (decision.hasMove()) {
        return decision.plan;
    }

    switch (decision.method) {
        case AgentDecisionMethod::NoLegalMove:
            throw std::runtime_error(
                "NGS agent found no legal move for the current board"
            );

        case AgentDecisionMethod::InvalidInput:
            throw std::runtime_error(
                "NGS agent rejected the board or player species as invalid"
            );

        case AgentDecisionMethod::Search:
        case AgentDecisionMethod::CandidateFallback:
        case AgentDecisionMethod::EmergencyFallback:
            throw std::runtime_error(
                "NGS agent returned an empty plan from a move-producing decision"
            );
    }

    throw std::runtime_error(
        "NGS agent returned an empty plan with an unknown decision method"
    );
}

} // namespace

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

    return requireDecisionPlan(decision);
}

vvw::game::TurnPlan NgsDecisionEngine::chooseMove(
    const vvw::game::BoardState& board,
    vvw::game::DecisionDeadline deadline
) {
    const AgentDecision decision = agent_.chooseMove(board, deadline);

    return requireDecisionPlan(decision);
}

} // namespace vvw::ngs