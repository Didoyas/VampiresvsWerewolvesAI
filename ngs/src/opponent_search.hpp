
#pragma once

#include "candidate_ranker.hpp"
#include "multi_move_candidate_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vvw::ngs {

struct OpponentSearchOptions {
    // Stage 3 settings used to rank and shortlist our initial candidates.
    CandidateRankingOptions rootRankingOptions{};

    // Stage 2 settings used to generate the opponent's responses.
    MultiMoveCandidateOptions opponentGenerationOptions{};

    // Number of our top-ranked candidates examined by this search.
    std::size_t maxRootCandidates{6};

    // Maximum number of opponent responses examined per simulated position.
    std::size_t maxOpponentResponses{24};

    // Number of random transition simulations per root candidate.
    std::size_t simulationsPerRootCandidate{2};

    // Final score = mean worst-response score - riskAversion * stddev.
    double riskAversion{0.0};

    // Reproducible random simulations.
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
    // Index into the original vector passed to searchTurnCandidates().
    std::size_t candidateIndex{0};

    // Average score after our opponent's strongest generated response.
    double meanWorstResponseScore{0.0};

    // Variation in the worst-response scores across simulations.
    double standardDeviation{0.0};

    // Final score used to order our root candidates.
    double rankingScore{0.0};

    // Number of simulations that completed successfully.
    std::size_t simulationsCompleted{0};

    // Total opponent responses examined across those simulations.
    std::size_t opponentResponsesExamined{0};
};

struct OpponentSearchStats {
    std::size_t rootCandidatesProvided{0};
    std::size_t rootCandidatesShortlisted{0};
    std::size_t rootCandidatesEvaluated{0};

    std::size_t rootSimulationsAttempted{0};
    std::size_t rootTransitionFailures{0};

    std::size_t opponentPositionsGenerated{0};
    std::size_t opponentGenerationFailures{0};

    std::size_t opponentResponsesGenerated{0};
    std::size_t opponentResponsesExamined{0};
    std::size_t opponentResponsesSkippedByBudget{0};

    std::size_t opponentTransitionFailures{0};
    std::size_t evaluationFailures{0};

    std::size_t simulationsWithoutOpponentResponse{0};
    std::size_t simulationsCompleted{0};
};

struct OpponentSearchResult {
    OpponentSearchError error{OpponentSearchError::None};

    // Sorted from best to worst for our species.
    std::vector<RootCandidateScore> ranked;

    // These fields refer to the original input candidate vector.
    bool hasBestCandidate{false};
    std::size_t bestCandidateIndex{0};

    OpponentSearchStats stats{};

    bool valid() const noexcept {
        return error == OpponentSearchError::None;
    }
};

// Performs a bounded search:
// 1. Rank our candidates using Stage 3.
// 2. Keep the strongest root candidates.
// 3. Generate opponent responses after each simulated root move.
// 4. Evaluate the resulting positions after the opponent's response.
// 5. Rank our candidates by their average worst-response score.
//
// The caller can retrieve the selected plan using:
// candidates[result.bestCandidateIndex]
OpponentSearchResult searchTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const OpponentSearchOptions& options = {}
);

} // namespace vvw::ngs
