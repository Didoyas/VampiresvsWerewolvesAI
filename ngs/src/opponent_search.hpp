#pragma once

#include "candidate_ranker.hpp"
#include "multi_move_candidate_generator.hpp"
#include "opponent_predictor.hpp"
#include "search_deadline.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vvw::ngs {

struct OpponentSearchOptions {
    // Stage 3: initial ranking of our root candidate plans.
    CandidateRankingOptions rootRankingOptions{};

    // Stage 2: generation of the opponent's possible responses.
    MultiMoveCandidateOptions opponentGenerationOptions{};

    // Maximum number of our candidate plans examined.
    std::size_t maxRootCandidates{6};

    // Total number of unique opponent responses examined
    // per root simulation.
    std::size_t maxOpponentResponses{24};

    // Maximum number selected using historical predictions.
    std::size_t maxLikelyOpponentResponses{16};

    // Maximum number selected through tactical threat ranking,
    // including responses that also belong to the prediction pool.
    std::size_t maxAdversarialOpponentResponses{24};

    std::size_t simulationsPerRootCandidate{2};

    // Final score = mean combined score - riskAversion * stddev.
    double riskAversion{0.0};

    // Combined expected and worst-case response weights.
    double expectedResponseWeight{0.70};
    double worstCaseResponseWeight{0.30};

    std::uint32_t baseRandomSeed{0x5EED1234u};
};

enum class OpponentSearchError {
    None,
    InvalidBoard,
    InvalidSpecies,
    EmptyCandidates,
    InvalidOptions,
    RootRankingFailed,
    NoValidRootCandidates,
    NoValidSearchResults
};

struct RootCandidateScore {
    // Index into the original root candidate vector.
    std::size_t candidateIndex{0};

    // Average of the worst evaluated response in each simulation.
    double meanWorstResponseScore{0.0};

    // Average of prediction-weighted expected response scores.
    double meanExpectedResponseScore{0.0};

    // Average of the combined expected/worst-case scores.
    double meanCombinedResponseScore{0.0};

    double standardDeviation{0.0};

    // Score used to rank this root candidate.
    double rankingScore{0.0};

    std::size_t simulationsCompleted{0};

    std::size_t opponentResponsesExamined{0};

    std::size_t predictedResponsesExamined{0};

    std::size_t adversarialResponsesExamined{0};
};

struct OpponentSearchStats {
    std::size_t rootCandidatesProvided{0};
    std::size_t rootCandidatesShortlisted{0};
    std::size_t rootRankingCandidatesProvided{0};
    std::size_t rootRankingCandidatesEvaluated{0};
    std::size_t rootRankingCandidatesSkippedByBudget{0};
    std::size_t rootCandidatesEvaluated{0};

    std::size_t rootSimulationsAttempted{0};
    std::size_t rootTransitionFailures{0};

    std::size_t opponentPositionsGenerated{0};
    std::size_t opponentGenerationFailures{0};

    std::size_t opponentResponsesGenerated{0};
    std::size_t opponentResponsesSkippedByBudget{0};

    // Counts of response entries selected by each mechanism, aggregated over
    // root simulations. The same candidate may contribute in multiple sims.
    std::size_t responsesSelectedByPrediction{0};
    std::size_t responsesSelectedForAdversarialCoverage{0};

    std::size_t opponentResponsesExamined{0};
    std::size_t opponentResponsesEvaluated{0};

    std::size_t opponentTransitionFailures{0};
    std::size_t evaluationFailures{0};

    std::size_t simulationsWithoutOpponentResponse{0};
    std::size_t simulationsCompleted{0};

    // Prediction lifecycle. predictionsUsed is retained for compatibility and
    // means a valid prediction object was passed into searchCore(). It does not
    // by itself mean that a response received positive support or affected the
    // expected-response calculation.
    bool predictionsUsed{false};
    bool predictionAttempted{false};
    bool predictionValid{false};
    // True once the response-search core has actually been entered.
    // Distinguishes a valid prediction from search skipped at the deadline.
    bool responseSearchAttempted{false};
    bool predictionFallbackUsed{false};
    OpponentPredictionError predictionError{
        OpponentPredictionError::None
    };
    OpponentPredictionStats predictionStats{};

    // Aggregated over response-selection calls (normally one per root
    // simulation). These are work counters, not globally unique response-plan
    // counts, and may exceed opponentResponsesGenerated for one simulation.
    std::size_t responseCandidatesWithPositivePredictionSupport{0};
    std::size_t responsesSelectedByBoth{0};

    // Sum of heuristic supports that actually contributed to weighted expected
    // scores, aggregated across root simulations. This is not a probability or
    // a calibrated confidence value.
    double predictionWeightUsed{0.0};
    std::size_t weightedExpectedEvaluations{0};
    std::size_t uniformExpectedEvaluations{0};

    // Boundary timings in microseconds, measured outside inner search loops.
    std::uint64_t predictionElapsedMicroseconds{0};
    std::uint64_t searchElapsedMicroseconds{0};
};

struct OpponentSearchResult {
    OpponentSearchError error{OpponentSearchError::None};
    // True when search stopped at the shared deadline. Any returned ranking
    // contains only root candidates fully evaluated before the deadline.
    bool timedOut{false};

    std::vector<RootCandidateScore> ranked;

    bool hasBestCandidate{false};
    std::size_t bestCandidateIndex{0};

    // Best fully evaluated root-ranking candidate. The agent may use this as
    // a one-ply fallback if the deeper opponent-response search times out.
    bool hasRootRankingFallbackCandidate{false};
    std::size_t rootRankingFallbackCandidateIndex{0};

    OpponentSearchStats stats{};

    // Non-None when the history-aware overload could not
    // obtain valid predictions and used tactical fallback.
    OpponentPredictionError predictionError{
        OpponentPredictionError::None
    };

    bool valid() const noexcept {
        return error == OpponentSearchError::None;
    }
};

// -------------------------------------------------------------
// Original history-independent API.
// Retained for compatibility and early-game fallback.
// -------------------------------------------------------------

OpponentSearchResult searchTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const OpponentSearchOptions& options = {},
    SearchDeadline deadline = SearchDeadline::max()
);

// -------------------------------------------------------------
// New history-aware API.
//
// The search invokes predictOpponentBehaviour() using the
// supplied history and current turn, then uses the resulting
// predictions to prioritise opponent responses.
// -------------------------------------------------------------

OpponentSearchResult searchTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const OpponentHistory& history,
    std::uint32_t currentTurn,
    const OpponentSearchOptions& options = {},
    SearchDeadline deadline = SearchDeadline::max()
);

} // namespace vvw::ngs
