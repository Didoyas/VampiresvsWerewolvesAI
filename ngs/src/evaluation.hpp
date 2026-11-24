#pragma once

#include "types.hpp"

#include <cstdint>

namespace vvw::ngs {

// Weights used to combine the evaluator's strategic features.
// These are initial heuristic values and should be tuned with self-play.
struct EvaluationWeights {
    double population{1.0};
    // Attraction to ordinary human cells, measured from the nearest group
    // capable of contesting that human population. A nearby singleton cannot
    // dominate the score for a large human cluster.
    double humanPotential{0.35};
    // Additional value for winning the race to active human houses. House
    // distance is measured only from groups large enough to convert the
    // house population deterministically.
    // Start conservatively; tune through controlled self-play.
    double housePotential{0.30};
    double threatExposure{1.0};
    double fragmentation{0.20};

    // A connected component containing fewer creatures than this is
    // considered a small, potentially ineffective group.
    Population minimumUsefulGroupPopulation{3};
};

enum class EvaluationError {
    None,
    InvalidBoard,
    InvalidPerspective,
    InvalidSideToMove,
    InvalidWeights
};

struct EvaluationFeatures {
    std::uint64_t ownPopulation{0};
    std::uint64_t opponentPopulation{0};

    // Positive values favour the perspective species.
    double populationAdvantage{0.0};
    double humanPotentialAdvantage{0.0};
    double housePotentialAdvantage{0.0};
    double threatAdvantage{0.0};
    double fragmentationAdvantage{0.0};

    double ownHumanPotential{0.0};
    double opponentHumanPotential{0.0};
    // Only groups with enough population to deterministically convert a
    // house's current human population contribute to house potential.
    double ownHousePotential{0.0};
    double opponentHousePotential{0.0};

    // Expected defenders lost in a one-ply attack by sideToMove.
    double expectedThreatenedPopulation{0.0};

    // Penalties for small disconnected groups.
    double ownFragmentationPenalty{0.0};
    double opponentFragmentationPenalty{0.0};

    // Horizon diagnostics. When horizonAware is false, the board supplied no
    // valid horizon and the evaluator uses the caller's original weights.
    bool horizonAware{false};
    double remainingHorizonFraction{1.0};
    double endgameUrgency{0.0};
    double effectivePopulationWeight{0.0};
    double effectiveHumanPotentialWeight{0.0};
    double effectiveHousePotentialWeight{0.0};
    double effectiveThreatExposureWeight{0.0};
    double effectiveFragmentationWeight{0.0};

    double totalScore{0.0};
};

struct EvaluationResult {
    EvaluationError error{EvaluationError::None};
    EvaluationFeatures features{};

    bool valid() const noexcept {
        return error == EvaluationError::None;
    }
};

// Evaluate a board from perspectiveSpecies' point of view.
// sideToMove is the species that can make the next move. It is required so
// threat estimates only model attacks that are immediately available to the
// next player. Cross-ply movement-footprint restrictions are not part of the
// assignment rules and are not applied.
// Higher scores are better for perspectiveSpecies.
EvaluationResult evaluatePosition(
    const BoardState& board,
    Species perspectiveSpecies,
    Species sideToMove,
    EvaluationWeights weights = {}
);

} // namespace vvw::ngs
