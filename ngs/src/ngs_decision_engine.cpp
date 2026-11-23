#include "ngs_decision_engine.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace vvw::ngs {
namespace {

bool diagnosticsEnabled() noexcept {
    // Resolve once per process. No per-turn environment lookup when disabled.
    static const bool enabled = []() noexcept {
        const char* value = std::getenv("VVW_NGS_DIAGNOSTICS");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

const char* predictionErrorName(OpponentPredictionError error) noexcept {
    switch (error) {
        case OpponentPredictionError::None: return "none";
        case OpponentPredictionError::InvalidBoard: return "invalid_board";
        case OpponentPredictionError::InvalidSpecies: return "invalid_species";
        case OpponentPredictionError::InvalidHistory: return "invalid_history";
        case OpponentPredictionError::EmptyHistory: return "empty_history";
        case OpponentPredictionError::HistoryBoardDimensionsMismatch:
            return "history_dimensions_mismatch";
        case OpponentPredictionError::InvalidTurn: return "invalid_turn";
        case OpponentPredictionError::InvalidOptions: return "invalid_options";
    }
    return "unknown";
}

const char* historyErrorName(OpponentHistoryError error) noexcept {
    switch (error) {
        case OpponentHistoryError::None: return "none";
        case OpponentHistoryError::InvalidConfiguration:
            return "invalid_configuration";
        case OpponentHistoryError::InvalidBoard: return "invalid_board";
        case OpponentHistoryError::InvalidSpecies: return "invalid_species";
        case OpponentHistoryError::InvalidTurn: return "invalid_turn";
        case OpponentHistoryError::BoardDimensionsChanged:
            return "dimensions_changed";
        case OpponentHistoryError::HistoryNotInitialized:
            return "not_initialized";
        case OpponentHistoryError::InvalidPosition: return "invalid_position";
        case OpponentHistoryError::InvalidMovement: return "invalid_movement";
        case OpponentHistoryError::InvalidHalfLife: return "invalid_half_life";
    }
    return "unknown";
}

const char* decisionMethodName(AgentDecisionMethod method) noexcept {
    switch (method) {
        case AgentDecisionMethod::Search: return "search";
        case AgentDecisionMethod::CandidateFallback: return "candidate_fallback";
        case AgentDecisionMethod::EmergencyFallback: return "emergency_fallback";
        case AgentDecisionMethod::NoLegalMove: return "no_legal_move";
        case AgentDecisionMethod::InvalidInput: return "invalid_input";
    }
    return "unknown";
}

const char* predictorStatus(const AgentDecision& decision) noexcept {
    const OpponentSearchStats& stats = decision.opponentSearchStats;
    if (!stats.predictionAttempted) return "not_attempted";
    if (!stats.predictionValid) {
        return stats.responseSearchAttempted
            ? "invalid"
            : "invalid_search_not_started";
    }
    if (!stats.responseSearchAttempted) return "valid_search_not_started";
    if (stats.weightedExpectedEvaluations > 0) return "weighted_eval";
    if (stats.responseCandidatesWithPositivePredictionSupport > 0 ||
        stats.responsesSelectedByPrediction > 0) {
        return "positive_support_not_weighted";
    }
    if (stats.opponentResponsesGenerated == 0) {
        return "valid_no_response_candidates";
    }
    return "valid_no_matches";
}

void logDecisionDiagnostics(const AgentDecision& decision) {
    if (!diagnosticsEnabled()) {
        return;
    }

    const OpponentSearchStats& search = decision.opponentSearchStats;
    const OpponentPredictionStats& prediction = search.predictionStats;

    std::ostringstream line;
    line << "[NGS_DIAG]"
         << " decision=" << decision.turnNumber
         << " method=" << decisionMethodName(decision.method)
         << " history_update_attempted="
         << (decision.historyUpdateAttempted ? 1 : 0)
         << " history_updated=" << (decision.historyUpdated ? 1 : 0)
         << " history_error="
         << historyErrorName(decision.historyUpdateError)
         << " history_observation=" << decision.historyObservationNumber
         << " snapshots=" << decision.historySnapshotsRetained
         << " confirmed_movements=" << decision.confirmedMovementObservations
         << " predictor_status=" << predictorStatus(decision)
         << " response_search_attempted="
         << (search.responseSearchAttempted ? 1 : 0)
         << " prediction_error="
         << predictionErrorName(search.predictionError)
         << " movement_obs_used=" << prediction.movementObservationsUsed
         << " direction_evidence_used="
         << (prediction.directionEvidenceUsed ? 1 : 0)
         << " house_trends_available=" << prediction.houseTrendsAvailable
         << " destinations_with_house_history_support="
         << prediction.destinationsWithHouseHistorySupport
         << " opponent_groups=" << prediction.opponentGroupsObserved
         << " groups_with_predictions=" << prediction.groupsWithPredictions
         << " neighbours_examined=" << prediction.neighbouringCellsExamined
         << " destinations_rejected="
         << prediction.destinationsRejectedByPreviousPly
         << " destinations_ranked=" << prediction.destinationsRanked
         << " response_candidates_with_support="
         << search.responseCandidatesWithPositivePredictionSupport
         << " selected_prediction=" << search.responsesSelectedByPrediction
         << " selected_adversarial="
         << search.responsesSelectedForAdversarialCoverage
         << " selected_both=" << search.responsesSelectedByBoth
         << " responses_examined=" << search.opponentResponsesExamined
         << " responses_evaluated=" << search.opponentResponsesEvaluated
         << " prediction_weight_used=" << std::setprecision(8)
         << search.predictionWeightUsed
         << " weighted_eval=" << search.weightedExpectedEvaluations
         << " uniform_eval=" << search.uniformExpectedEvaluations
         << " prediction_fallback=" << (search.predictionFallbackUsed ? 1 : 0)
         << " history_update_us=" << decision.historyUpdateMicroseconds
         << " candidate_generation_us="
         << decision.candidateGenerationMicroseconds
         << " predictor_us=" << search.predictionElapsedMicroseconds
         << " response_search_us=" << search.searchElapsedMicroseconds
         << " candidates_generated=" << decision.candidatesGenerated
         << " root_candidates=" << decision.rootCandidatesEvaluated
         << " selected_move_count=" << decision.plan.moves.size();

    // Include the exact chosen plan in diagnostics. This is intentionally
    // behind VVW_NGS_DIAGNOSTICS so normal competition logs stay unchanged.
    for (std::size_t i = 0; i < decision.plan.moves.size(); ++i) {
        const vvw::game::Move& move = decision.plan.moves[i];
        line << " selected_move_" << i
             << '=' << move.source.x << ',' << move.source.y
             << ':' << move.count
             << "->" << move.destination.x << ',' << move.destination.y;
    }

    line << " deadline_exceeded=" << (decision.deadlineExceeded ? 1 : 0)
         << " elapsed_us=" << decision.elapsedMicroseconds;

    std::cerr << line.str() << '\n';
}

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
    logDecisionDiagnostics(decision);
    return requireDecisionPlan(decision);
}

vvw::game::TurnPlan NgsDecisionEngine::chooseMove(
    const vvw::game::BoardState& board,
    vvw::game::DecisionDeadline deadline
) {
    const AgentDecision decision = agent_.chooseMove(board, deadline);
    logDecisionDiagnostics(decision);
    return requireDecisionPlan(decision);
}

} // namespace vvw::ngs