
#pragma once

#include "opponent_history.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vvw::ngs {

struct DirectionPrediction {
    // Direction relative to the source position.
    // Each component is -1, 0 or 1.
    int dx{0};
    int dy{0};

    std::size_t observations{0};

    double weightedObservations{0.0};

    // Smoothed relative frequency across the eight directions.
    double probability{0.0};
};

struct PredictedDestination {
    Position source{};
    Position destination{};

    Population sourcePopulation{0};

    int dx{0};
    int dy{0};

    double directionProbability{0.0};
    double destinationPresenceFrequency{0.0};
    double houseProgressScore{0.0};

    // Weighted heuristic score, not a calibrated probability.
    double priorityScore{0.0};

    // Rank within this source group's predicted destinations.
    std::size_t rankWithinSource{0};
};

struct OpponentPredictionOptions {
    double halfLifeTurns{8.0};

    // Laplace-style smoothing for each of the eight directions.
    double directionSmoothing{0.5};

    std::size_t maxDestinationsPerGroup{3};

    double directionWeight{0.45};
    double destinationHistoryWeight{0.20};
    double houseProgressWeight{0.35};

    // Apply the previous-ply source/destination restrictions
    // when BoardState contains a known previous-ply footprint.
    bool respectPreviousPlyRestrictions{true};
};

enum class OpponentPredictionError {
    None,
    InvalidBoard,
    InvalidSpecies,
    InvalidHistory,
    EmptyHistory,
    HistoryBoardDimensionsMismatch,
    InvalidTurn,
    InvalidOptions
};

struct OpponentPredictionStats {
    std::size_t opponentGroupsObserved{0};
    std::size_t groupsWithPredictions{0};

    std::size_t neighbouringCellsExamined{0};
    std::size_t destinationsRejectedByPreviousPly{0};

    std::size_t movementObservationsUsed{0};
    std::size_t destinationsRanked{0};
};

struct OpponentPredictionResult {
    OpponentPredictionError error{
        OpponentPredictionError::None
    };

    std::vector<DirectionPrediction> directions;

    // Predictions are grouped by source position.
    // Each source's destinations are sorted by priorityScore.
    std::vector<PredictedDestination> destinations;

    // Retained original historical house analysis.
    std::vector<HouseTrend> houseTrends;

    OpponentPredictionStats stats{};

    bool valid() const noexcept {
        return error == OpponentPredictionError::None;
    }
};

// Predicts plausible neighbouring destinations for each currently
// observed opponent group.
//
// This is a heuristic predictor based on:
// 1. Recency-weighted confirmed movement directions.
// 2. Historical occupancy of destination cells.
// 3. Progress towards currently active human houses.
//
// It does not infer individual group identities or claim that
// a predicted destination is a guaranteed future move.
OpponentPredictionResult predictOpponentBehaviour(
    const BoardState& board,
    const OpponentHistory& history,
    std::uint32_t currentTurn,
    const OpponentPredictionOptions& options = {}
);

} // namespace vvw::ngs
