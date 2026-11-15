
#include "opponent_history.hpp"
#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace vvw::ngs {
namespace {

bool isPlayableSpecies(Species species) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

Population opponentPopulationFor(
    const Cell& cell,
    Species species
) noexcept {
    switch (species) {
        case Species::Vampires:
            return cell.vampires;

        case Species::Werewolves:
            return cell.werewolves;

        case Species::Unknown:
            return 0;
    }

    return 0;
}

const OpponentPositionObservation* findPosition(
    const OpponentSnapshot& snapshot,
    Position position
) noexcept {
    for (const OpponentPositionObservation& observation :
         snapshot.positions) {
        if (observation.position == position) {
            return &observation;
        }
    }

    return nullptr;
}

} // namespace

OpponentHistory::OpponentHistory(
    Species opponentSpecies,
    std::size_t maxSnapshots
)
    : opponentSpecies_(opponentSpecies),
      maxSnapshots_(maxSnapshots) {
}

bool OpponentHistory::isConfigured() const noexcept {
    return isPlayableSpecies(opponentSpecies_) &&
           maxSnapshots_ > 0;
}

Species OpponentHistory::opponentSpecies() const noexcept {
    return opponentSpecies_;
}

bool OpponentHistory::hasObservations() const noexcept {
    return !snapshots_.empty();
}

std::size_t OpponentHistory::snapshotCount() const noexcept {
    return snapshots_.size();
}

std::uint32_t OpponentHistory::latestTurn() const noexcept {
    if (snapshots_.empty()) {
        return 0;
    }

    return snapshots_.back().turn;
}

int OpponentHistory::rows() const noexcept {
    return rows_;
}

int OpponentHistory::columns() const noexcept {
    return columns_;
}

const std::vector<OpponentSnapshot>&
OpponentHistory::snapshots() const noexcept {
    return snapshots_;
}

const std::vector<OpponentMovementObservation>&
OpponentHistory::confirmedMovements() const noexcept {
    return confirmedMovements_;
}

bool OpponentHistory::inBounds(Position position) const noexcept {
    return dimensionsInitialized_ &&
           position.x >= 0 &&
           position.y >= 0 &&
           position.x < columns_ &&
           position.y < rows_;
}

bool OpponentHistory::isAdjacent(
    Position source,
    Position destination
) const noexcept {
    const int dx = std::abs(destination.x - source.x);
    const int dy = std::abs(destination.y - source.y);

    return (dx <= 1 && dy <= 1) &&
           (dx != 0 || dy != 0);
}

HistoryUpdateResult OpponentHistory::recordObservation(
    const BoardState& board,
    std::uint32_t turn
) {
    HistoryUpdateResult result;

    if (!isConfigured()) {
        result.error = OpponentHistoryError::InvalidConfiguration;
        return result;
    }

    if (!board.hasValidStorage()) {
        result.error = OpponentHistoryError::InvalidBoard;
        return result;
    }

    if (!isPlayableSpecies(board.ourSpecies) ||
        opponentOf(board.ourSpecies) != opponentSpecies_) {
        result.error = OpponentHistoryError::InvalidSpecies;
        return result;
    }

    if (!snapshots_.empty() && turn <= latestTurn()) {
        result.error = OpponentHistoryError::InvalidTurn;
        return result;
    }

    if (dimensionsInitialized_ &&
        (board.rows != rows_ || board.columns != columns_)) {
        result.error = OpponentHistoryError::BoardDimensionsChanged;
        return result;
    }

    OpponentSnapshot snapshot;
    snapshot.turn = turn;

    // Capture the currently observed opponent populations.
    // Empty cells are intentionally not stored.
    for (int y = 0; y < board.rows; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            const Position position{x, y};
            const Cell& cell = board.at(position);

            const Population population =
                opponentPopulationFor(cell, opponentSpecies_);

            if (population == 0) {
                continue;
            }

            snapshot.positions.push_back({
                position,
                population
            });

            snapshot.totalPopulation +=
                static_cast<std::uint64_t>(population);
        }
    }

    result.positionsObserved = snapshot.positions.size();

    // Initialize the board dimensions after a valid observation
    // has been completely constructed.
    if (!dimensionsInitialized_) {
        rows_ = board.rows;
        columns_ = board.columns;
        dimensionsInitialized_ = true;
    }

    snapshots_.push_back(std::move(snapshot));

    // Retain the newest maxSnapshots observations.
    if (snapshots_.size() > maxSnapshots_) {
        snapshots_.erase(snapshots_.begin());
    }

    result.snapshotsRetained = snapshots_.size();

    return result;
}

OpponentHistoryError OpponentHistory::recordConfirmedMovement(
    std::uint32_t turn,
    Position source,
    Position destination,
    Population count
) {
    if (!isConfigured()) {
        return OpponentHistoryError::InvalidConfiguration;
    }

    if (!dimensionsInitialized_) {
        return OpponentHistoryError::HistoryNotInitialized;
    }

    if (!inBounds(source) || !inBounds(destination)) {
        return OpponentHistoryError::InvalidPosition;
    }

    if (!isAdjacent(source, destination) ||
        count == 0 ||
        count > 255) {
        return OpponentHistoryError::InvalidMovement;
    }

    // Preserve chronological order of confirmed movement events.
    // Multiple movements on the same turn are allowed.
    if (hasMovementTurn_ && turn < latestMovementTurn_) {
        return OpponentHistoryError::InvalidTurn;
    }

    confirmedMovements_.push_back({
        turn,
        source,
        destination,
        count
    });

    latestMovementTurn_ = turn;
    hasMovementTurn_ = true;

    return OpponentHistoryError::None;
}

PositionStatisticsResult OpponentHistory::getPositionStatistics(
    Position position,
    std::uint32_t currentTurn,
    double halfLifeTurns
) const {
    PositionStatisticsResult result;

    if (!isConfigured()) {
        result.error = OpponentHistoryError::InvalidConfiguration;
        return result;
    }

    if (!dimensionsInitialized_ || snapshots_.empty()) {
        result.error = OpponentHistoryError::HistoryNotInitialized;
        return result;
    }

    if (!inBounds(position)) {
        result.error = OpponentHistoryError::InvalidPosition;
        return result;
    }

    if (!std::isfinite(halfLifeTurns) || halfLifeTurns <= 0.0) {
        result.error = OpponentHistoryError::InvalidHalfLife;
        return result;
    }

    if (currentTurn < latestTurn()) {
        result.error = OpponentHistoryError::InvalidTurn;
        return result;
    }

    OpponentPositionStatistics& statistics =
        result.statistics;

    statistics.snapshotsConsidered = snapshots_.size();

    double totalRecencyWeight = 0.0;

    for (std::size_t i = 0; i < snapshots_.size(); ++i) {
        const OpponentSnapshot& snapshot = snapshots_[i];

        const std::uint32_t age =
            currentTurn - snapshot.turn;

        const double weight =
            std::exp2(
                -static_cast<double>(age) / halfLifeTurns
            );

        totalRecencyWeight += weight;

        const OpponentPositionObservation* observation =
            findPosition(snapshot, position);

        if (observation == nullptr) {
            continue;
        }

        ++statistics.presenceCount;

        statistics.hasBeenObserved = true;
        statistics.lastSeenTurn = snapshot.turn;

        statistics.maximumObservedPopulation =
            std::max(
                statistics.maximumObservedPopulation,
                observation->population
            );

        statistics.recencyWeightedPresence += weight;

        // The newest retained snapshot is the final element.
        if (i == snapshots_.size() - 1) {
            statistics.latestPopulation = observation->population;
        }
    }

    if (statistics.snapshotsConsidered > 0) {
        statistics.presenceFrequency =
            static_cast<double>(statistics.presenceCount) /
            static_cast<double>(statistics.snapshotsConsidered);
    }

    if (totalRecencyWeight > 0.0) {
        statistics.recencyWeightedPresenceFrequency =
            statistics.recencyWeightedPresence /
            totalRecencyWeight;
    }

    return result;
}

} // namespace vvw::ngs
