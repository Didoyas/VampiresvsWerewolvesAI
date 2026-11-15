
#include "opponent_history.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <limits>

namespace vvw::ngs {

OpponentHistory::OpponentHistory(
    Species opponentSpecies,
    std::size_t maxSnapshots
)
    : opponentSpecies_(opponentSpecies),
      maxSnapshots_(maxSnapshots) {
}

bool OpponentHistory::isPlayableSpecies(
    Species species
) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

bool OpponentHistory::isConfigured() const noexcept {
    return maxSnapshots_ > 0;
}

Species OpponentHistory::opponentSpecies() const noexcept {
    return opponentSpecies_;
}

void OpponentHistory::clear() {
    snapshots_.clear();
    confirmedMovements_.clear();

    rows_ = 0;
    columns_ = 0;
    dimensionsInitialized_ = false;

    hasMovementTurn_ = false;
    latestMovementTurn_ = 0;
}

std::size_t OpponentHistory::size() const noexcept {
    return snapshots_.size();
}

bool OpponentHistory::empty() const noexcept {
    return snapshots_.empty();
}

std::size_t OpponentHistory::capacity() const noexcept {
    return maxSnapshots_;
}

const std::vector<BoardSnapshot>&
OpponentHistory::snapshots() const noexcept {
    return snapshots_;
}

const std::vector<OpponentMovementObservation>&
OpponentHistory::confirmedMovements() const noexcept {
    return confirmedMovements_;
}

const BoardSnapshot& OpponentHistory::newest(
    std::size_t age
) const {
    if (age >= snapshots_.size()) {
        throw std::out_of_range(
            "OpponentHistory: requested snapshot is not available"
        );
    }

    return snapshots_[snapshots_.size() - 1 - age];
}

bool OpponentHistory::inBounds(
    Position position
) const noexcept {
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
    const int dx = absInt(destination.x - source.x);
    const int dy = absInt(destination.y - source.y);

    return dx <= 1 &&
           dy <= 1 &&
           (dx != 0 || dy != 0);
}

int OpponentHistory::absInt(int value) noexcept {
    return value < 0 ? -value : value;
}

double OpponentHistory::clamp01(double value) noexcept {
    if (value < 0.0) {
        return 0.0;
    }

    if (value > 1.0) {
        return 1.0;
    }

    return value;
}

Population OpponentHistory::populationFor(
    const HistoryCell& cell,
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

// -------------------------------------------------------------
// Snapshot recording
// -------------------------------------------------------------

void OpponentHistory::record(
    std::size_t width,
    std::size_t height,
    const std::vector<HistoryCell>& cells,
    std::uint32_t update_number
) {
    if (!isConfigured()) {
        throw std::invalid_argument(
            "OpponentHistory: invalid snapshot capacity"
        );
    }

    if (width == 0 ||
        height == 0 ||
        width > static_cast<std::size_t>(-1) / height ||
        cells.size() != width * height) {
        throw std::invalid_argument(
            "OpponentHistory: invalid board dimensions/cell count"
        );
    }

    if (width > static_cast<std::size_t>(
                    std::numeric_limits<int>::max()) ||
        height > static_cast<std::size_t>(
                     std::numeric_limits<int>::max())) {
        throw std::invalid_argument(
            "OpponentHistory: board dimensions exceed supported range"
        );
    }

    if (!snapshots_.empty() &&
        update_number <= snapshots_.back().update_number) {
        throw std::invalid_argument(
            "OpponentHistory: update number must increase"
        );
    }

    if (dimensionsInitialized_ &&
        (static_cast<int>(height) != rows_ ||
         static_cast<int>(width) != columns_)) {
        throw std::invalid_argument(
            "OpponentHistory: board dimensions changed"
        );
    }

    BoardSnapshot snapshot;

    snapshot.width = width;
    snapshot.height = height;
    snapshot.update_number = update_number;
    snapshot.turn = update_number;
    snapshot.cells = cells;

    if (!dimensionsInitialized_) {
        rows_ = static_cast<int>(height);
        columns_ = static_cast<int>(width);
        dimensionsInitialized_ = true;
    }

    snapshots_.push_back(std::move(snapshot));

    // Retain the newest maxSnapshots_ snapshots.
    if (snapshots_.size() > maxSnapshots_) {
        snapshots_.erase(snapshots_.begin());
    }
}

HistoryUpdateResult OpponentHistory::recordObservation(
    const BoardState& board,
    std::uint32_t turn
) {
    HistoryUpdateResult result;

    if (!isConfigured()) {
        result.error =
            OpponentHistoryError::InvalidConfiguration;
        return result;
    }

    if (!board.hasValidStorage()) {
        result.error =
            OpponentHistoryError::InvalidBoard;
        return result;
    }

    if (!isPlayableSpecies(opponentSpecies_)) {
        result.error = OpponentHistoryError::InvalidSpecies;
        return result; 
    }

    if (!isPlayableSpecies(board.ourSpecies) ||
        opponentOf(board.ourSpecies) != opponentSpecies_) {
        result.error =
            OpponentHistoryError::InvalidSpecies;
        return result;
    }

    if (!snapshots_.empty() &&
        turn <= snapshots_.back().turn) {
        result.error =
            OpponentHistoryError::InvalidTurn;
        return result;
    }

    if (dimensionsInitialized_ &&
        (board.rows != rows_ ||
         board.columns != columns_)) {
        result.error =
            OpponentHistoryError::BoardDimensionsChanged;
        return result;
    }

    std::vector<HistoryCell> cells;
    cells.reserve(board.cells.size());

    for (const Cell& cell : board.cells) {
        cells.emplace_back(
            cell.humans,
            cell.vampires,
            cell.werewolves,
            cell.isHouse
        );
    }

    record(
        static_cast<std::size_t>(board.columns),
        static_cast<std::size_t>(board.rows),
        cells,
        turn
    );

    for (const HistoryCell& cell : cells) {
        if (populationFor(cell, opponentSpecies_) > 0) {
            ++result.positionsObserved;
        }
    }

    result.snapshotsRetained = snapshots_.size();

    return result;
}

// -------------------------------------------------------------
// Original house approach analysis
// -------------------------------------------------------------

std::vector<HouseTrend> OpponentHistory::houseApproachTrends(
    Species opponent
) const {
    std::vector<HouseTrend> result;

    if (!isPlayableSpecies(opponent) ||
        snapshots_.size() < 2) {
        return result;
    }

    const BoardSnapshot& current = newest(0);

    // Each entry contains enemy positions from one snapshot,
    // ordered chronologically from oldest to newest.
    std::vector<std::vector<Position>> enemyPositions;
    enemyPositions.reserve(snapshots_.size());

    for (const BoardSnapshot& snapshot : snapshots_) {
        std::vector<Position> positions;

        for (std::size_t index = 0;
             index < snapshot.cells.size();
             ++index) {

            const HistoryCell& cell = snapshot.cells[index];

            if (populationFor(cell, opponent) == 0) {
                continue;
            }

            Position position;

            position.x = static_cast<int>(
                index % snapshot.width
            );

            position.y = static_cast<int>(
                index / snapshot.width
            );

            positions.push_back(position);
        }

        enemyPositions.push_back(std::move(positions));
    }

    const std::size_t transitions =
        snapshots_.size() - 1;

    for (std::size_t houseIndex = 0;
         houseIndex < current.cells.size();
         ++houseIndex) {

        const HistoryCell& houseCell =
            current.cells[houseIndex];

        // Only currently active human houses are considered.
        if (!houseCell.is_house || houseCell.humans == 0) {
            continue;
        }

        const int houseX = static_cast<int>(
            houseIndex % current.width
        );

        const int houseY = static_cast<int>(
            houseIndex / current.width
        );

        std::vector<int> distances;
        distances.reserve(snapshots_.size());

        bool allSnapshotsHaveEnemy = true;

        for (const std::vector<Position>& positions :
             enemyPositions) {

            int nearestDistance = -1;

            for (const Position& enemy : positions) {
                const int dx = absInt(enemy.x - houseX);
                const int dy = absInt(enemy.y - houseY);

                // Chebyshev distance: movement in 8 directions.
                const int distance = std::max(dx, dy);

                if (nearestDistance < 0 ||
                    distance < nearestDistance) {
                    nearestDistance = distance;
                }
            }

            if (nearestDistance < 0) {
                allSnapshotsHaveEnemy = false;
                break;
            }

            distances.push_back(nearestDistance);
        }

        // If the opponent disappeared from the reconstructed
        // board at any observation, the trend is ambiguous.
        if (!allSnapshotsHaveEnemy ||
            distances.size() != snapshots_.size()) {
            continue;
        }

        std::size_t approachingTransitions = 0;

        for (std::size_t i = 1; i < distances.size(); ++i) {
            if (distances[i] < distances[i - 1]) {
                ++approachingTransitions;
            }
        }

        const int netDistanceClosed =
            distances.front() - distances.back();

        const double positiveNet =
            netDistanceClosed > 0
                ? static_cast<double>(netDistanceClosed)
                : 0.0;

        const double averageApproach =
            positiveNet / static_cast<double>(transitions);

        const double confidence =
            static_cast<double>(approachingTransitions) /
            static_cast<double>(transitions);

        // A single ordinary move can close Chebyshev distance
        // by at most one. Clamp apparent strength conservatively.
        const double strength = clamp01(averageApproach);

        HouseTrend trend;

        trend.cell_index = houseIndex;
        trend.oldest_distance = distances.front();
        trend.newest_distance = distances.back();
        trend.average_approach_per_turn = averageApproach;
        trend.confidence = confidence;
        trend.risk = strength * confidence;

        result.push_back(trend);
    }

    return result;
}

double OpponentHistory::applyHouseHistoryDiscount(
    double baseHouseValue,
    double historyRisk,
    bool contested,
    double maxDiscount
) noexcept {
    if (!contested || baseHouseValue <= 0.0) {
        return baseHouseValue;
    }

    const double safeDiscount = clamp01(maxDiscount);
    const double safeRisk = clamp01(historyRisk);

    return baseHouseValue *
           (1.0 - safeDiscount * safeRisk);
}

// -------------------------------------------------------------
// Confirmed movement history
// -------------------------------------------------------------

OpponentHistoryError OpponentHistory::recordConfirmedMovement(
    std::uint32_t turn,
    Position source,
    Position destination,
    Population count
) {
    if (!isConfigured()) {
        return OpponentHistoryError::InvalidConfiguration;
    }

    if (!isPlayableSpecies(opponentSpecies_)) {
        return OpponentHistoryError::InvalidSpecies;
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

    if (hasMovementTurn_ &&
        turn < latestMovementTurn_) {
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

// -------------------------------------------------------------
// Historical position statistics
// -------------------------------------------------------------

PositionStatisticsResult
OpponentHistory::getPositionStatistics(
    Position position,
    std::uint32_t currentTurn,
    double halfLifeTurns
) const {
    PositionStatisticsResult result;


    if (!isPlayableSpecies(opponentSpecies_)) {
        result.error = OpponentHistoryError::InvalidSpecies;
        return result;
    }

    if (!isConfigured()) {
        result.error =
            OpponentHistoryError::InvalidConfiguration;
        return result;
    }

    if (snapshots_.empty() || !dimensionsInitialized_) {
        result.error =
            OpponentHistoryError::HistoryNotInitialized;
        return result;
    }

    if (!inBounds(position)) {
        result.error =
            OpponentHistoryError::InvalidPosition;
        return result;
    }

    if (!std::isfinite(halfLifeTurns) ||
        halfLifeTurns <= 0.0) {
        result.error =
            OpponentHistoryError::InvalidHalfLife;
        return result;
    }

    if (currentTurn < snapshots_.back().turn) {
        result.error =
            OpponentHistoryError::InvalidTurn;
        return result;
    }

    OpponentPositionStatistics& statistics =
        result.statistics;

    statistics.snapshotsConsidered = snapshots_.size();

    double totalRecencyWeight = 0.0;

    const std::size_t positionIndex =
        static_cast<std::size_t>(position.y) *
            static_cast<std::size_t>(columns_) +
        static_cast<std::size_t>(position.x);

    for (std::size_t i = 0; i < snapshots_.size(); ++i) {
        const BoardSnapshot& snapshot = snapshots_[i];

        const std::uint32_t age =
            currentTurn - snapshot.turn;

        const double weight =
            std::exp2(
                -static_cast<double>(age) / halfLifeTurns
            );

        totalRecencyWeight += weight;

        const HistoryCell& cell =
            snapshot.cells[positionIndex];

        const Population population =
            populationFor(cell, opponentSpecies_);

        if (population == 0) {
            continue;
        }

        ++statistics.presenceCount;

        statistics.hasBeenObserved = true;
        statistics.lastSeenTurn = snapshot.turn;

        statistics.maximumObservedPopulation =
            std::max(
                statistics.maximumObservedPopulation,
                population
            );

        statistics.recencyWeightedPresence += weight;

        if (i == snapshots_.size() - 1) {
            statistics.latestPopulation = population;
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
