
#include "opponent_predictor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace vvw::ngs {
namespace {

struct DirectionAccumulator {
    int dx{0};
    int dy{0};

    std::size_t observations{0};
    double weightedObservations{0.0};
};

struct ActiveHouse {
    int x{0};
    int y{0};
};

const std::array<Position, 8> kDirections{{
    {-1, -1},
    { 0, -1},
    { 1, -1},
    {-1,  0},
    { 1,  0},
    {-1,  1},
    { 0,  1},
    { 1,  1}
}};

bool isPlayableSpecies(Species species) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

Population opponentPopulation(
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

bool validOptions(
    const OpponentPredictionOptions& options,
    bool hasActiveHouses,
    bool hasHouseHistory,
    bool hasMovementEvidence
) noexcept {
    if (!std::isfinite(options.halfLifeTurns) ||
        options.halfLifeTurns <= 0.0) {
        return false;
    }

    if (!std::isfinite(options.directionSmoothing) ||
        options.directionSmoothing <= 0.0) {
        return false;
    }

    if (options.maxDestinationsPerGroup == 0) {
        return false;
    }

    const double weights[] = {
        options.directionWeight,
        options.destinationHistoryWeight,
        options.houseProgressWeight,
        options.houseHistoryWeight
    };

    for (double weight : weights) {
        if (!std::isfinite(weight) || weight < 0.0) {
            return false;
        }
    }

    const double availableWeight =
        (hasMovementEvidence ? options.directionWeight : 0.0) +
        options.destinationHistoryWeight +
        (hasActiveHouses ? options.houseProgressWeight : 0.0) +
        (hasHouseHistory ? options.houseHistoryWeight : 0.0);

    return std::isfinite(availableWeight) &&
           availableWeight > 0.0;
}

int chebyshevDistance(
    int x1,
    int y1,
    int x2,
    int y2
) noexcept {
    return std::max(
        std::abs(x1 - x2),
        std::abs(y1 - y2)
    );
}

int nearestHouseDistance(
    int x,
    int y,
    const std::vector<ActiveHouse>& houses
) noexcept {
    int nearest = std::numeric_limits<int>::max();

    for (const ActiveHouse& house : houses) {
        nearest = std::min(
            nearest,
            chebyshevDistance(
                x,
                y,
                house.x,
                house.y
            )
        );
    }

    return nearest;
}

int findDirectionIndex(int dx, int dy) noexcept {
    for (std::size_t i = 0; i < kDirections.size(); ++i) {
        if (kDirections[i].x == dx &&
            kDirections[i].y == dy) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

std::vector<DirectionPrediction> calculateDirectionPredictions(
    const OpponentHistory& history,
    std::uint32_t currentTurn,
    const OpponentPredictionOptions& options
) {
    std::array<DirectionAccumulator, 8> accumulators{};

    for (std::size_t i = 0; i < kDirections.size(); ++i) {
        accumulators[i].dx = kDirections[i].x;
        accumulators[i].dy = kDirections[i].y;
    }

    double totalWeightedObservations = 0.0;

    for (const OpponentMovementObservation& movement :
         history.confirmedMovements()) {

        // Defensive guard: never let unsigned subtraction turn a future-dated
        // event into an apparently ancient observation.
        if (movement.turn > currentTurn) {
            continue;
        }

        const int dx =
            movement.destination.x - movement.source.x;

        const int dy =
            movement.destination.y - movement.source.y;

        const int directionIndex =
            findDirectionIndex(dx, dy);

        if (directionIndex < 0) {
            continue;
        }

        const std::uint32_t age =
            currentTurn - movement.turn;

        const double weight =
            std::exp2(
                -static_cast<double>(age) /
                options.halfLifeTurns
            );

        DirectionAccumulator& accumulator =
            accumulators[static_cast<std::size_t>(directionIndex)];

        ++accumulator.observations;
        accumulator.weightedObservations += weight;

        totalWeightedObservations += weight;
    }

    const double smoothingTotal =
        8.0 * options.directionSmoothing;

    const double denominator =
        totalWeightedObservations + smoothingTotal;

    std::vector<DirectionPrediction> predictions;
    predictions.reserve(8);

    for (const DirectionAccumulator& accumulator :
         accumulators) {

        DirectionPrediction prediction;

        prediction.dx = accumulator.dx;
        prediction.dy = accumulator.dy;
        prediction.observations = accumulator.observations;
        prediction.weightedObservations =
            accumulator.weightedObservations;

        prediction.probability =
            (accumulator.weightedObservations +
             options.directionSmoothing) /
            denominator;

        predictions.push_back(prediction);
    }

    return predictions;
}

std::vector<ActiveHouse> findActiveHouses(
    const BoardState& board
) {
    std::vector<ActiveHouse> houses;

    for (int y = 0; y < board.rows; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            const Cell& cell = board.at({x, y});

            if (!cell.isHouse || cell.humans == 0) {
                continue;
            }

            houses.push_back({x, y});
        }
    }

    return houses;
}

double calculateHouseProgress(
    Position source,
    Position destination,
    const std::vector<ActiveHouse>& houses
) noexcept {
    if (houses.empty()) {
        return 0.0;
    }

    const int sourceDistance =
        nearestHouseDistance(
            source.x,
            source.y,
            houses
        );

    const int destinationDistance =
        nearestHouseDistance(
            destination.x,
            destination.y,
            houses
        );

    // The score is 1 when the destination is closer to
    // at least one nearest-house target, otherwise 0.
    return destinationDistance < sourceDistance
        ? 1.0
        : 0.0;
}

// Uses the existing snapshot-derived house trends as a conservative target
// preference. This does not infer an exact opponent movement. A candidate
// receives support only when it moves closer to a currently active house
// whose nearest-opponent distance has historically tended to decrease.
double calculateHouseHistoryScore(
    Position source,
    Position destination,
    const std::vector<HouseTrend>& trends,
    int boardColumns
) noexcept {
    double bestScore = 0.0;

    if (boardColumns <= 0) {
        return bestScore;
    }

    for (const HouseTrend& trend : trends) {
        if (!std::isfinite(trend.risk) || trend.risk <= 0.0) {
            continue;
        }

        const std::size_t houseXIndex =
            trend.cell_index % static_cast<std::size_t>(boardColumns);
        const std::size_t houseYIndex =
            trend.cell_index / static_cast<std::size_t>(boardColumns);

        const int houseX = static_cast<int>(houseXIndex);
        const int houseY = static_cast<int>(houseYIndex);

        const int sourceDistance = chebyshevDistance(
            source.x, source.y, houseX, houseY
        );
        const int destinationDistance = chebyshevDistance(
            destination.x, destination.y, houseX, houseY
        );

        if (destinationDistance < sourceDistance) {
            bestScore = std::max(
                bestScore,
                std::max(0.0, std::min(1.0, trend.risk))
            );
            if (bestScore >= 1.0) {
                break;
            }
        }
    }

    return bestScore;
}

double calculatePriorityScore(
    double directionProbability,
    double destinationPresenceFrequency,
    double houseProgressScore,
    double houseHistoryScore,
    const OpponentPredictionOptions& options,
    bool hasActiveHouses,
    bool hasHouseHistory,
    bool hasMovementEvidence
) noexcept {
    // Do not treat Laplace-smoothed uniform directions as behavioural evidence.
    const double directionWeight =
        hasMovementEvidence ? options.directionWeight : 0.0;

    const double destinationWeight =
        options.destinationHistoryWeight;

    const double houseWeight =
        hasActiveHouses
            ? options.houseProgressWeight
            : 0.0;

    const double houseHistoryWeight =
        hasHouseHistory
            ? options.houseHistoryWeight
            : 0.0;

    const double totalWeight =
        directionWeight +
        destinationWeight +
        houseWeight +
        houseHistoryWeight;

    return (
        directionWeight * directionProbability +
        destinationWeight * destinationPresenceFrequency +
        houseWeight * houseProgressScore +
        houseHistoryWeight * houseHistoryScore
    ) / totalWeight;
}

} // namespace

OpponentPredictionResult predictOpponentBehaviour(
    const BoardState& board,
    const OpponentHistory& history,
    std::uint32_t currentTurn,
    const OpponentPredictionOptions& options
) {
    OpponentPredictionResult result;

    // ---------------------------------------------------------
    // Step 1: Validate current board.
    // ---------------------------------------------------------

    if (!board.hasValidStorage()) {
        result.error = OpponentPredictionError::InvalidBoard;
        return result;
    }

    if (!isPlayableSpecies(board.ourSpecies)) {
        result.error = OpponentPredictionError::InvalidSpecies;
        return result;
    }

    const Species opponentSpecies =
        opponentOf(board.ourSpecies);

    if (!isPlayableSpecies(opponentSpecies) ||
        history.opponentSpecies() != opponentSpecies) {
        result.error = OpponentPredictionError::InvalidSpecies;
        return result;
    }

    if (!history.isConfigured()) {
        result.error = OpponentPredictionError::InvalidHistory;
        return result;
    }

    // ---------------------------------------------------------
    // Step 2: Require historical observations.
    // ---------------------------------------------------------

    if (history.empty() || history.snapshots().empty()) {
        result.error = OpponentPredictionError::EmptyHistory;
        return result;
    }

    const BoardSnapshot& latestSnapshot =
        history.snapshots().back();

    if (latestSnapshot.width !=
            static_cast<std::size_t>(board.columns) ||
        latestSnapshot.height !=
            static_cast<std::size_t>(board.rows)) {
        result.error =
            OpponentPredictionError::HistoryBoardDimensionsMismatch;
        return result;
    }

    if (currentTurn < latestSnapshot.turn) {
        result.error = OpponentPredictionError::InvalidTurn;
        return result;
    }

    for (const OpponentMovementObservation& movement :
         history.confirmedMovements()) {

        if (movement.turn > currentTurn) {
            result.error = OpponentPredictionError::InvalidTurn;
            return result;
        }
    }

    // ---------------------------------------------------------
    // Step 3: Find active human houses and validate options.
    // ---------------------------------------------------------

    const std::vector<ActiveHouse> houses =
        findActiveHouses(board);

    const bool hasActiveHouses = !houses.empty();
    const bool hasMovementEvidence = !history.confirmedMovements().empty();
    result.stats.movementObservationsUsed =
        history.confirmedMovements().size();
    result.stats.directionEvidenceUsed = hasMovementEvidence;

    // ---------------------------------------------------------
    // Step 4: Retain snapshot-derived house approach information.
    // ---------------------------------------------------------

    result.houseTrends =
        history.houseApproachTrends(opponentSpecies);

    const bool hasHouseHistory = std::any_of(
        result.houseTrends.begin(),
        result.houseTrends.end(),
        [](const HouseTrend& trend) {
            return std::isfinite(trend.risk) && trend.risk > 0.0;
        }
    );

    result.stats.houseTrendsAvailable = static_cast<std::size_t>(
        std::count_if(
            result.houseTrends.begin(),
            result.houseTrends.end(),
            [](const HouseTrend& trend) {
                return std::isfinite(trend.risk) && trend.risk > 0.0;
            }
        )
    );

    if (!validOptions(
            options, hasActiveHouses, hasHouseHistory, hasMovementEvidence)) {
        result.error = OpponentPredictionError::InvalidOptions;
        return result;
    }

    // ---------------------------------------------------------
    // Step 5: Derive direction statistics from confirmed moves.
    // ---------------------------------------------------------

    result.directions =
        calculateDirectionPredictions(
            history,
            currentTurn,
            options
        );

    // ---------------------------------------------------------
    // Step 6: Enumerate currently observed opponent groups.
    // ---------------------------------------------------------

    for (int y = 0; y < board.rows; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            const Position source{x, y};

            const Population population =
                opponentPopulation(
                    board.at(source),
                    opponentSpecies
                );

            if (population == 0) {
                continue;
            }

            ++result.stats.opponentGroupsObserved;

            std::vector<PredictedDestination> groupPredictions;

            // -------------------------------------------------
            // Step 7: Examine all eight neighbouring cells.
            // -------------------------------------------------

            for (const Position& direction : kDirections) {
                ++result.stats.neighbouringCellsExamined;

                const Position destination{
                    x + direction.x,
                    y + direction.y
                };

                if (!board.inBounds(destination)) {
                    continue;
                }

                const int directionIndex =
                    findDirectionIndex(
                        direction.x,
                        direction.y
                    );

                if (directionIndex < 0) {
                    continue;
                }

                // -------------------------------------------------
                // Step 8: Obtain direction prior.
                // -------------------------------------------------

                const double directionProbability =
                    result.directions[
                        static_cast<std::size_t>(directionIndex)
                    ].probability;

                // -------------------------------------------------
                // Step 9: Obtain historical destination preference.
                // -------------------------------------------------

                const PositionStatisticsResult destinationStats =
                    history.getPositionStatistics(
                        destination,
                        currentTurn,
                        options.halfLifeTurns
                    );

                if (!destinationStats.valid()) {
                    result.error =
                        OpponentPredictionError::InvalidHistory;
                    result.destinations.clear();
                    return result;
                }

                const double presenceFrequency =
                    destinationStats.statistics
                        .recencyWeightedPresenceFrequency;

                // -------------------------------------------------
                // Step 10: Estimate progress towards human houses.
                // -------------------------------------------------

                const double houseProgress =
                    calculateHouseProgress(
                        source,
                        destination,
                        houses
                    );

                const double houseHistoryScore =
                    calculateHouseHistoryScore(
                        source,
                        destination,
                        result.houseTrends,
                        board.columns
                    );

                if (houseHistoryScore > 0.0) {
                    ++result.stats.destinationsWithHouseHistorySupport;
                }

                // -------------------------------------------------
                // Step 11: Combine the heuristic scores.
                // -------------------------------------------------

                PredictedDestination prediction;

                prediction.source = source;
                prediction.destination = destination;
                prediction.sourcePopulation = population;

                prediction.dx = direction.x;
                prediction.dy = direction.y;  

                prediction.directionProbability =
                    directionProbability;

                prediction.destinationPresenceFrequency =
                    presenceFrequency;

                prediction.houseProgressScore =
                    houseProgress;

                prediction.houseHistoryScore =
                    houseHistoryScore;

                prediction.priorityScore =
                    calculatePriorityScore(
                        directionProbability,
                        presenceFrequency,
                        houseProgress,
                        houseHistoryScore,
                        options,
                        hasActiveHouses,
                        hasHouseHistory,
                        hasMovementEvidence
                    );

                groupPredictions.push_back(prediction);
            }

            // -------------------------------------------------
            // Step 12: Rank destinations for this source group.
            // -------------------------------------------------

            std::sort(
                groupPredictions.begin(),
                groupPredictions.end(),
                [](const PredictedDestination& lhs,
                   const PredictedDestination& rhs) {

                    if (lhs.priorityScore != rhs.priorityScore) {
                        return lhs.priorityScore > rhs.priorityScore;
                    }

                    if (lhs.destination.y != rhs.destination.y) {
                        return lhs.destination.y < rhs.destination.y;
                    }

                    return lhs.destination.x < rhs.destination.x;
                }
            );

            const std::size_t countToKeep =
                std::min(
                    options.maxDestinationsPerGroup,
                    groupPredictions.size()
                );

            if (countToKeep > 0) {
                ++result.stats.groupsWithPredictions;
            }

            for (std::size_t rank = 0; rank < countToKeep; ++rank) {
                groupPredictions[rank].rankWithinSource = rank + 1;

                result.destinations.push_back(
                    groupPredictions[rank]
                );

                ++result.stats.destinationsRanked;
            }
        }
    }

    return result;
}

} // namespace vvw::ngs
