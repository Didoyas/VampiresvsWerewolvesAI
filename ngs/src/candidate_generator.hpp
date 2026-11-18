#pragma once

#include "movement_rules.hpp"
#include "search_deadline.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vvw::ngs {

// A fractional movement amount. The generated count is rounded up to the
// nearest integer so that small groups still produce a non-zero candidate.
// Valid ratios satisfy: 0 < numerator <= denominator.
struct SplitRatio {
    std::uint32_t numerator{1};
    std::uint32_t denominator{4};
};

struct CandidateGenerationOptions {
    // Default portfolio: move one quarter, one half, three quarters, or all
    // of the source group. Counts are capped at the one-byte MOV limit (255).
    std::vector<SplitRatio> splitRatios{
        {1, 4},
        {1, 2},
        {3, 4},
        {1, 1}
    };

    // Forwarded to the existing movement validator. Keep true in production.
    // Set false only for isolated tests where the previous ply is unavailable.
    bool requirePreviousPlyFootprint{true};
};

enum class CandidateGenerationError {
    None,
    InvalidBoard,
    InvalidSpecies,
    MissingPreviousPlyFootprint,
    InvalidSplitRatio
};

struct CandidateGenerationStats {
    std::size_t sourceGroupsExamined{0};
    std::size_t neighbourCellsExamined{0};
    std::size_t candidatesGenerated{0};
    std::size_t candidatesRejectedByValidator{0};
    std::size_t duplicateCountsSkipped{0};
};

struct CandidateGenerationResult {
    CandidateGenerationError error{CandidateGenerationError::None};
    std::vector<TurnPlan> candidates;
    CandidateGenerationStats stats;
    // True when generation stopped cooperatively at the shared deadline.
    // A timed-out result may still contain valid, usable partial candidates.
    bool timedOut{false};

    bool valid() const noexcept {
        return error == CandidateGenerationError::None;
    }
};

// Stage 1: generate legal plans containing exactly one movement from one
// source cell to one of its eight neighbouring cells.
//
// Every generated plan is passed through validateTurnPlan(). This function
// does not combine movements from different source cells; that is Stage 2.
CandidateGenerationResult generateSingleMoveCandidates(
    const BoardState& board,
    Species actingSpecies,
    const CandidateGenerationOptions& options = {},
    SearchDeadline deadline = SearchDeadline::max()
);

} // namespace vvw::ngs
