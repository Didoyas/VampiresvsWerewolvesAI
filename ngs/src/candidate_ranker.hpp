#pragma once

#include "evaluation.hpp"
#include "search_deadline.hpp"
#include "transition_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vvw::ngs {

// Controls how candidates are simulated and evaluated.
struct CandidateRankingOptions {
    EvaluationWeights evaluationWeights{};

    TransitionOptions transitionOptions{};

    // Maximum number of simulations for each candidate.
    // Deterministic candidates normally need only one.
    std::size_t simulationsPerCandidate{4};

    // Bound the amount of work within the turn deadline.
    // When there are more candidates, they are sampled evenly
    // across the input list.
    std::size_t maxCandidatesToEvaluate{96};

    // 0.0 = rank by mean score only.
    // Higher values penalize uncertain outcomes.
    double riskAversion{0.0};

    // Fixed seed makes ranking reproducible during testing.
    std::uint32_t baseRandomSeed{0xC0FFEEu};
};

enum class CandidateRankingError {
    None,
    InvalidBoard,
    InvalidSpecies,
    EmptyCandidates,
    InvalidOptions,
    NoValidCandidates
};

// Score and statistics for one candidate.
// candidateIndex refers to the original input vector.
struct CandidateScore {
    std::size_t candidateIndex{0};

    double meanScore{0.0};
    double standardDeviation{0.0};

    // The score used for sorting:
    // meanScore - riskAversion * standardDeviation
    double rankingScore{0.0};

    std::size_t simulationsCompleted{0};
};

struct CandidateRankingStats {
    std::size_t candidatesProvided{0};
    std::size_t candidatesSelectedForEvaluation{0};
    std::size_t candidatesSkippedByBudget{0};

    std::size_t candidatesEvaluated{0};

    std::size_t deterministicCandidates{0};
    std::size_t stochasticCandidates{0};

    std::size_t transitionFailures{0};
    std::size_t evaluationFailures{0};

    std::size_t simulationsAttempted{0};
    std::size_t simulationsSucceeded{0};
    std::size_t simulationsFailed{0};
};

struct CandidateRankingResult {
    CandidateRankingError error{CandidateRankingError::None};
    // If true, ranked contains only candidates completed before the deadline.
    bool timedOut{false};

    // Sorted in descending rankingScore order.
    // ranked.front() is the best candidate.
    std::vector<CandidateScore> ranked;

    CandidateRankingStats stats;

    bool valid() const noexcept {
        return error == CandidateRankingError::None;
    }
};

// Stage 3:
// Simulate and evaluate complete candidate plans.
// Higher scores favour actingSpecies.
CandidateRankingResult rankTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const CandidateRankingOptions& options = {},
    SearchDeadline deadline = SearchDeadline::max()
);

} // namespace vvw::ngs