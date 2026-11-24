
#pragma once

#include "candidate_generator.hpp"
#include "search_deadline.hpp"

#include <cstddef>
#include <vector>

namespace vvw::ngs {

// Stage 2 builds complete turn candidates from three strategic movement
// patterns: splitting one source group across distinct destinations, merging
// multiple source groups at a shared destination, and general multi-source
// plans. Every plan remains subject to the same authoritative validator.
struct MultiMoveCandidateOptions {
    CandidateGenerationOptions singleMoveOptions{};

    // Keep the branching factor small enough for the later search layer.
    // Supported values are 2, 3, or 4.
    std::size_t maxMovesPerPlan{3};

    // Per-plan-size quotas. Zero disables that candidate class.
    std::size_t maxSingleMoveCandidates{48};
    std::size_t maxTwoMoveCandidates{128};
    std::size_t maxThreeMoveCandidates{64};
    std::size_t maxFourMoveCandidates{0};

    // Special two-move plans are generated first within the shared
    // maxTwoMoveCandidates budget. They do not increase the overall pair
    // branching factor beyond that budget.
    std::size_t maxSplitCandidates{12};
    std::size_t maxMergeCandidates{12};

    // Number of deterministic movement combinations sampled for each
    // selected set of source cells.
    std::size_t variantsPerSourceCombination{2};

    // Hard cap on attempted compound plans, including plans rejected by the
    // validator. This prevents pathological boards from causing unbounded work.
    std::size_t maxCombinationAttempts{20000};
};

enum class MultiMoveCandidateError {
    None,
    InvalidBoard,
    InvalidSpecies,
    InvalidSplitRatio,
    InvalidOptions
};

struct MultiMoveCandidateStats {
    std::size_t singleMoveCandidatesAvailable{0};
    std::size_t sourceGroupsAvailable{0};

    std::size_t singleMoveCandidatesIncluded{0};
    // Number of distinct source groups represented in the bounded single-move
    // shortlist (one representative per group before second choices are added).
    std::size_t singleMoveSourceGroupsCovered{0};
    std::size_t twoMoveCandidatesGenerated{0};
    std::size_t splitPlansGenerated{0};
    std::size_t mergePlansGenerated{0};
    // Board-level coordination decisions used to guide candidate priorities.
    std::size_t groupsAssignedToHouses{0};
    std::size_t distinctHouseTargetsAssigned{0};
    std::size_t threatenedGroupsDetected{0};
    std::size_t reinforcementMovesPrioritized{0};
    std::size_t threeMoveCandidatesGenerated{0};
    std::size_t fourMoveCandidatesGenerated{0};

    std::size_t combinationAttempts{0};
    std::size_t combinationsRejectedByValidator{0};
    std::size_t duplicatePlansSkipped{0};
    bool combinationBudgetExhausted{false};
};

struct MultiMoveCandidateResult {
    MultiMoveCandidateError error{MultiMoveCandidateError::None};
    std::vector<TurnPlan> candidates;
    MultiMoveCandidateStats stats;

    // Partial candidates remain valid when generation reaches the deadline.
    bool timedOut{false};

    bool valid() const noexcept {
        return error == MultiMoveCandidateError::None;
    }
};

// Generates a bounded portfolio containing single-, two-, three-, and
// optionally four-movement plans. Every returned compound plan is checked
// by validateTurnPlan().
MultiMoveCandidateResult generateMultiMoveCandidates(
    const BoardState& board,
    Species actingSpecies,
    const MultiMoveCandidateOptions& options = {},
    SearchDeadline deadline = SearchDeadline::max()
);

} // namespace vvw::ngs
