#pragma once

#include "types.hpp"

#include <cstdint>

namespace vvw::ngs {

// Weights used to combine the evaluator's strategic features.
// These are initial heuristic values and should be tuned with self-play.
struct EvaluationWeights {
    double population{1.0};
    double humanPotential{0.35};
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
    double threatAdvantage{0.0};
    double fragmentationAdvantage{0.0};

    double ownHumanPotential{0.0};
    double opponentHumanPotential{0.0};

    // Expected defenders lost in a one-ply attack by sideToMove.
    double expectedThreatenedPopulation{0.0};

    // Penalties for small disconnected groups.
    double ownFragmentationPenalty{0.0};
    double opponentFragmentationPenalty{0.0};

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
// next player and can apply the previous-ply movement footprint correctly.
// Higher scores are better for perspectiveSpecies.
EvaluationResult evaluatePosition(
    const BoardState& board,
    Species perspectiveSpecies,
    Species sideToMove,
    EvaluationWeights weights = {}
);

} // namespace vvw::ngs
