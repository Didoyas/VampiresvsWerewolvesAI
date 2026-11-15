
#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vvw::ngs {

// One opponent stack observed at a position in a particular snapshot.
// This is an observation, not an inferred movement.
struct OpponentPositionObservation {
    Position position{};
    Population population{0};
};

// One reconstructed board observation.
struct OpponentSnapshot {
    std::uint32_t turn{0};

    // Total population of the opponent across the board.
    std::uint64_t totalPopulation{0};

    // Only occupied opponent positions are stored.
    std::vector<OpponentPositionObservation> positions;
};

// A movement explicitly confirmed by protocol information.
// Snapshot differences are never automatically converted into these records.
struct OpponentMovementObservation {
    std::uint32_t turn{0};
    Position source{};
    Position destination{};
    Population count{0};
};

enum class OpponentHistoryError {
    None,
    InvalidConfiguration,
    InvalidBoard,
    InvalidSpecies,
    InvalidTurn,
    BoardDimensionsChanged,
    HistoryNotInitialized,
    InvalidPosition,
    InvalidMovement,
    InvalidHalfLife
};

struct HistoryUpdateResult {
    OpponentHistoryError error{OpponentHistoryError::None};

    std::size_t positionsObserved{0};
    std::size_t snapshotsRetained{0};

    bool valid() const noexcept {
        return error == OpponentHistoryError::None;
    }
};

struct OpponentPositionStatistics {
    std::size_t snapshotsConsidered{0};

    // Number of retained snapshots in which this position was occupied.
    std::size_t presenceCount{0};

    // presenceCount / snapshotsConsidered.
    double presenceFrequency{0.0};

    // Sum of recency weights for snapshots in which this position
    // was occupied.
    double recencyWeightedPresence{0.0};

    // Recency-weighted presence divided by total snapshot weight.
    double recencyWeightedPresenceFrequency{0.0};

    // Population at this position in the latest retained snapshot.
    Population latestPopulation{0};

    Population maximumObservedPopulation{0};

    bool hasBeenObserved{false};
    std::uint32_t lastSeenTurn{0};
};

struct PositionStatisticsResult {
    OpponentHistoryError error{OpponentHistoryError::None};
    OpponentPositionStatistics statistics{};

    bool valid() const noexcept {
        return error == OpponentHistoryError::None;
    }
};

class OpponentHistory {
public:
    // The history belongs to one opponent species.
    //
    // maxSnapshots defaults to 256. This is sufficient to retain
    // the expected full game history under the current 100-ply
    // per-side limit, assuming one observation per ply.
    explicit OpponentHistory(
        Species opponentSpecies,
        std::size_t maxSnapshots = 256
    );

    bool isConfigured() const noexcept;

    Species opponentSpecies() const noexcept;

    bool hasObservations() const noexcept;

    std::size_t snapshotCount() const noexcept;

    std::uint32_t latestTurn() const noexcept;

    int rows() const noexcept;

    int columns() const noexcept;

    const std::vector<OpponentSnapshot>& snapshots() const noexcept;

    const std::vector<OpponentMovementObservation>&
    confirmedMovements() const noexcept;

    // Store the opponent's observed positions from a reconstructed
    // board snapshot.
    //
    // Turn numbers must increase strictly.
    // Board dimensions must remain constant during a game.
    HistoryUpdateResult recordObservation(
        const BoardState& board,
        std::uint32_t turn
    );

    // Store a movement only when the protocol explicitly confirms it.
    //
    // The movement must be within board bounds, adjacent under the
    // game's 8-neighbour movement rule, and have a count from 1 to 255.
    OpponentHistoryError recordConfirmedMovement(
        std::uint32_t turn,
        Position source,
        Position destination,
        Population count
    );

    // Calculate historical occupancy statistics for one position.
    //
    // currentTurn must be greater than or equal to the latest
    // recorded snapshot turn. halfLifeTurns must be positive.
    PositionStatisticsResult getPositionStatistics(
        Position position,
        std::uint32_t currentTurn,
        double halfLifeTurns = 8.0
    ) const;

private:
    Species opponentSpecies_{Species::Unknown};

    std::size_t maxSnapshots_{0};

    int rows_{0};
    int columns_{0};
    bool dimensionsInitialized_{false};

    bool hasMovementTurn_{false};
    std::uint32_t latestMovementTurn_{0};

    std::vector<OpponentSnapshot> snapshots_;

    std::vector<OpponentMovementObservation> confirmedMovements_;

    bool inBounds(Position position) const noexcept;

    bool isAdjacent(Position source, Position destination) const noexcept;
};

} // namespace vvw::ngs
