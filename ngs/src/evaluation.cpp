#include "evaluation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vvw::ngs {
namespace {

bool isPlayableSpecies(Species species) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

Population populationFor(const Cell& cell, Species species) noexcept {
    return species == Species::Vampires
        ? cell.vampires
        : cell.werewolves;
}

std::uint64_t totalPopulation(
    const BoardState& board,
    Species species
) noexcept {
    std::uint64_t total = 0;
    for (const Cell& cell : board.cells) {
        total += static_cast<std::uint64_t>(populationFor(cell, species));
    }
    return total;
}

// Multi-source breadth-first search over the board's eight-neighbour grid.
// The result is the minimum number of legal one-cell movement steps to a
// cell occupied by the requested species. Cells are not blocked by humans or
// other creatures; this is a strategic distance estimate, not pathfinding.
std::vector<int> buildDistanceMap(
    const BoardState& board,
    Species species,
    Population minimumGroupPopulation
) {
    const std::size_t count = board.cells.size();
    std::vector<int> distance(count, -1);
    std::vector<std::size_t> queue;
    queue.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        if (populationFor(board.cells[i], species) >= minimumGroupPopulation) {
            distance[i] = 0;
            queue.push_back(i);
        }
    }

    std::size_t head = 0;
    while (head < queue.size()) {
        const std::size_t current = queue[head++];
        const int x = static_cast<int>(current %
                                       static_cast<std::size_t>(board.columns));
        const int y = static_cast<int>(current /
                                       static_cast<std::size_t>(board.columns));

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }

                const int nextX = x + dx;
                const int nextY = y + dy;
                if (nextX < 0 || nextY < 0 ||
                    nextX >= board.columns || nextY >= board.rows) {
                    continue;
                }

                const std::size_t next =
                    static_cast<std::size_t>(nextY) *
                        static_cast<std::size_t>(board.columns) +
                    static_cast<std::size_t>(nextX);

                if (distance[next] != -1) {
                    continue;
                }

                distance[next] = distance[current] + 1;
                queue.push_back(next);
            }
        }
    }

    return distance;
}

double calculateHumanPotential(
    const BoardState& board,
    const std::vector<int>& distance
) noexcept {
    double potential = 0.0;

    for (std::size_t i = 0; i < board.cells.size(); ++i) {
        const Population humans = board.cells[i].humans;
        if (humans == 0 || distance[i] < 0) {
            continue;
        }

        // One step away has value 1/2, two steps 1/3, and so on.
        const double closeness =
            1.0 / (1.0 + static_cast<double>(distance[i]));
        potential += static_cast<double>(humans) * closeness;
    }

    return potential;
}

// Houses are strategic objectives. Unlike ordinary human potential, a house
// should not appear close to being secured merely because a small detached
// group is nearby. Only groups large enough to convert the house population
// deterministically contribute to this objective score. Smaller groups can
// still win a random encounter, but that uncertain outcome is not credited as
// guaranteed capture potential here.
double calculateHousePotential(
    const BoardState& board,
    Species species
) {
    double potential = 0.0;
    std::unordered_map<Population, std::vector<int>> distancesByHousePopulation;

    for (std::size_t i = 0; i < board.cells.size(); ++i) {
        const Cell& target = board.cells[i];
        if (!target.isHouse || target.humans == 0) {
            continue;
        }

        auto distanceIt = distancesByHousePopulation.find(target.humans);
        if (distanceIt == distancesByHousePopulation.end()) {
            auto distances = buildDistanceMap(board, species, target.humans);
            distanceIt = distancesByHousePopulation.emplace(
                target.humans, std::move(distances)
            ).first;
        }

        const int distance = distanceIt->second[i];
        if (distance < 0) {
            continue;
        }

        const double closeness =
            1.0 / (1.0 + static_cast<double>(distance));
        potential += static_cast<double>(target.humans) * closeness;
    }

    return potential;
}

// Returns the sum of populations in disconnected components that are below
// the configured useful-group threshold. Components use eight-neighbour
// connectivity because diagonal movement is allowed by the game.
double calculateFragmentationPenalty(
    const BoardState& board,
    Species species,
    Population minimumUsefulGroupPopulation
) {
    const std::size_t count = board.cells.size();
    std::vector<unsigned char> visited(count, 0);
    std::vector<std::size_t> queue;
    queue.reserve(count);

    double penalty = 0.0;

    for (std::size_t start = 0; start < count; ++start) {
        if (visited[start] != 0 ||
            populationFor(board.cells[start], species) == 0) {
            continue;
        }

        queue.clear();
        queue.push_back(start);
        visited[start] = 1;

        std::uint64_t componentPopulation = 0;
        std::size_t head = 0;

        while (head < queue.size()) {
            const std::size_t current = queue[head++];
            componentPopulation += static_cast<std::uint64_t>(
                populationFor(board.cells[current], species));

            const int x = static_cast<int>(current %
                                           static_cast<std::size_t>(board.columns));
            const int y = static_cast<int>(current /
                                           static_cast<std::size_t>(board.columns));

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }

                    const int nextX = x + dx;
                    const int nextY = y + dy;
                    if (nextX < 0 || nextY < 0 ||
                        nextX >= board.columns || nextY >= board.rows) {
                        continue;
                    }

                    const std::size_t next =
                        static_cast<std::size_t>(nextY) *
                            static_cast<std::size_t>(board.columns) +
                        static_cast<std::size_t>(nextX);

                    if (visited[next] != 0 ||
                        populationFor(board.cells[next], species) == 0) {
                        continue;
                    }

                    visited[next] = 1;
                    queue.push_back(next);
                }
            }
        }

        if (componentPopulation <
            static_cast<std::uint64_t>(minimumUsefulGroupPopulation)) {
            penalty += static_cast<double>(componentPopulation);
        }
    }

    return penalty;
}

bool attackersReachDeterministicWin(
    std::uint64_t attackers,
    std::uint64_t defenders
) noexcept {
    return 2ULL * attackers >= 3ULL * defenders;
}

bool defendersReachDeterministicWin(
    std::uint64_t attackers,
    std::uint64_t defenders
) noexcept {
    return 2ULL * defenders >= 3ULL * attackers;
}

double attackerWinProbabilityWide(
    std::uint64_t attackers,
    std::uint64_t defenders
) noexcept {
    if (attackers == 0) {
        return 0.0;
    }
    if (defenders == 0) {
        return 1.0;
    }

    double probability = 0.0;
    if (attackers == defenders) {
        probability = 0.5;
    } else if (attackers < defenders) {
        probability = static_cast<double>(attackers) /
                      (2.0 * static_cast<double>(defenders));
    } else {
        probability = static_cast<double>(attackers) /
                      static_cast<double>(defenders) - 0.5;
    }

    return std::clamp(probability, 0.0, 1.0);
}

// Estimates the defender casualties from the next player's best local
// opportunity at each defending cell. Adjacent eligible attacker groups are
// aggregated, matching the transition engine's aggregated-arrival model.
double calculateExpectedThreatenedPopulation(
    const BoardState& board,
    Species attackingSpecies
) noexcept {
    const Species defendingSpecies = opponentOf(attackingSpecies);
    double totalExpectedLoss = 0.0;

    for (int y = 0; y < board.rows; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            const Position defendedPosition{x, y};
            const std::size_t defendedIndex = board.index(defendedPosition);
            const Population defendersPopulation = populationFor(
                board.cells[defendedIndex], defendingSpecies);

            if (defendersPopulation == 0) {
                continue;
            }

            // The next player cannot move into a cell that was a source in
            // the immediately preceding ply.
            if (board.previousPly.known &&
                board.previousPly.wasSource(defendedPosition)) {
                continue;
            }

            std::uint64_t adjacentAttackers = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }

                    const Position attackerPosition{x + dx, y + dy};
                    if (!board.inBounds(attackerPosition)) {
                        continue;
                    }

                    // A cell that was a destination in the preceding ply
                    // cannot be used as a source in the next ply.
                    if (board.previousPly.known &&
                        board.previousPly.wasDestination(attackerPosition)) {
                        continue;
                    }

                    const Population group = populationFor(
                        board.at(attackerPosition), attackingSpecies);
                    adjacentAttackers += static_cast<std::uint64_t>(group);
                }
            }

            if (adjacentAttackers == 0) {
                continue;
            }

            const std::uint64_t defenders =
                static_cast<std::uint64_t>(defendersPopulation);

            double attackerWinProbability = 0.0;
            if (attackersReachDeterministicWin(
                    adjacentAttackers, defenders)) {
                attackerWinProbability = 1.0;
            } else if (defendersReachDeterministicWin(
                           adjacentAttackers, defenders)) {
                attackerWinProbability = 0.0;
            } else {
                attackerWinProbability = attackerWinProbabilityWide(
                    adjacentAttackers, defenders);
            }

            // In the current transition model, if the attacker wins the
            // defenders are eliminated. If the attacker loses, each defender
            // survives with probability (1 - p). Expected defender survivors
            // are therefore D * (1 - p)^2.
            const double defenderCount = static_cast<double>(defenders);
            const double defenderSurvival =
                1.0 - attackerWinProbability;
            const double expectedSurvivors =
                defenderCount * defenderSurvival * defenderSurvival;

            totalExpectedLoss += defenderCount - expectedSurvivors;
        }
    }

    return totalExpectedLoss;
}

bool validWeights(const EvaluationWeights& weights) noexcept {
    return std::isfinite(weights.population) &&
           std::isfinite(weights.humanPotential) &&
           std::isfinite(weights.housePotential) &&
           std::isfinite(weights.threatExposure) &&
           std::isfinite(weights.fragmentation) &&
           weights.population >= 0.0 &&
           weights.humanPotential >= 0.0 &&
           weights.housePotential >= 0.0 &&
           weights.threatExposure >= 0.0 &&
           weights.fragmentation >= 0.0;
}

struct EffectiveEvaluationWeights {
    double population{0.0};
    double humanPotential{0.0};
    double housePotential{0.0};
    double threatExposure{0.0};
    double fragmentation{0.0};
    double remainingHorizonFraction{1.0};
    double endgameUrgency{0.0};
    bool horizonAware{false};
};

EffectiveEvaluationWeights effectiveWeightsFor(
    const BoardState& board,
    const EvaluationWeights& base
) noexcept {
    EffectiveEvaluationWeights result;
    result.population = base.population;
    result.humanPotential = base.humanPotential;
    result.housePotential = base.housePotential;
    result.threatExposure = base.threatExposure;
    result.fragmentation = base.fragmentation;

    // A default or malformed horizon must not silently masquerade as an
    // early-game observation. Keep the exact legacy weights in that case.
    if (!board.horizon.hasValidValues()) {
        return result;
    }

    const double ourPliesRemaining =
        static_cast<double>(board.horizon.ourPliesRemaining()) /
        static_cast<double>(GameHorizon::kMaxPliesPerSide);
    const double opponentPliesRemaining =
        static_cast<double>(board.horizon.opponentPliesRemaining()) /
        static_cast<double>(GameHorizon::kMaxPliesPerSide);
    const double timeRemaining = std::clamp(
        board.horizon.secondsRemaining() / GameHorizon::kMaxGameSeconds,
        0.0,
        1.0);

    // Whichever limit is closest governs the effective planning horizon.
    result.remainingHorizonFraction = std::clamp(
        std::min({ourPliesRemaining, opponentPliesRemaining, timeRemaining}),
        0.0,
        1.0);
    result.endgameUrgency = 1.0 - result.remainingHorizonFraction;
    result.horizonAware = true;

    // Smoothly pivot away from long-term expansion/positioning and toward
    // immediate material and tactical safety as either game limit approaches.
    result.population = base.population *
        (1.0 + 0.50 * result.endgameUrgency);
    result.humanPotential = base.humanPotential *
        (1.0 - 0.75 * result.endgameUrgency);
    result.housePotential = base.housePotential *
        (1.0 - 0.75 * result.endgameUrgency);
    result.threatExposure = base.threatExposure *
        (1.0 + 0.40 * result.endgameUrgency);
    result.fragmentation = base.fragmentation *
        (1.0 - 0.50 * result.endgameUrgency);

    return result;
}

} // namespace

EvaluationResult evaluatePosition(
    const BoardState& board,
    Species perspectiveSpecies,
    Species sideToMove,
    EvaluationWeights weights
) {
    EvaluationResult result;

    if (!board.hasValidStorage()) {
        result.error = EvaluationError::InvalidBoard;
        return result;
    }

    if (!isPlayableSpecies(perspectiveSpecies)) {
        result.error = EvaluationError::InvalidPerspective;
        return result;
    }

    if (!isPlayableSpecies(sideToMove)) {
        result.error = EvaluationError::InvalidSideToMove;
        return result;
    }

    if (!validWeights(weights)) {
        result.error = EvaluationError::InvalidWeights;
        return result;
    }

    const Species opponentSpecies = opponentOf(perspectiveSpecies);

    result.features.ownPopulation =
        totalPopulation(board, perspectiveSpecies);
    result.features.opponentPopulation =
        totalPopulation(board, opponentSpecies);

    result.features.populationAdvantage =
        static_cast<double>(result.features.ownPopulation) -
        static_cast<double>(result.features.opponentPopulation);

    // Keep ordinary human attraction based on the nearest group, as before.
    // House attraction is stricter: only a group large enough to guarantee
    // conversion is considered. This prevents a nearby singleton from
    // masking a main force's opportunity to approach a viable house.
    const std::vector<int> ownDistance =
        buildDistanceMap(board, perspectiveSpecies, 1);
    const std::vector<int> opponentDistance =
        buildDistanceMap(board, opponentSpecies, 1);

    result.features.ownHumanPotential =
        calculateHumanPotential(board, ownDistance);
    result.features.opponentHumanPotential =
        calculateHumanPotential(board, opponentDistance);
    result.features.humanPotentialAdvantage =
        result.features.ownHumanPotential -
        result.features.opponentHumanPotential;

    result.features.ownHousePotential =
        calculateHousePotential(board, perspectiveSpecies);
    result.features.opponentHousePotential =
        calculateHousePotential(board, opponentSpecies);
    result.features.housePotentialAdvantage =
        result.features.ownHousePotential -
        result.features.opponentHousePotential;

    result.features.expectedThreatenedPopulation =
        calculateExpectedThreatenedPopulation(board, sideToMove);
    result.features.threatAdvantage =
        sideToMove == perspectiveSpecies
            ? result.features.expectedThreatenedPopulation
            : -result.features.expectedThreatenedPopulation;

    result.features.ownFragmentationPenalty =
        calculateFragmentationPenalty(
            board,
            perspectiveSpecies,
            weights.minimumUsefulGroupPopulation);
    result.features.opponentFragmentationPenalty =
        calculateFragmentationPenalty(
            board,
            opponentSpecies,
            weights.minimumUsefulGroupPopulation);
    result.features.fragmentationAdvantage =
        result.features.opponentFragmentationPenalty -
        result.features.ownFragmentationPenalty;

    const EffectiveEvaluationWeights effectiveWeights =
        effectiveWeightsFor(board, weights);

    result.features.horizonAware = effectiveWeights.horizonAware;
    result.features.remainingHorizonFraction =
        effectiveWeights.remainingHorizonFraction;
    result.features.endgameUrgency = effectiveWeights.endgameUrgency;
    result.features.effectivePopulationWeight = effectiveWeights.population;
    result.features.effectiveHumanPotentialWeight =
        effectiveWeights.humanPotential;
    result.features.effectiveHousePotentialWeight =
        effectiveWeights.housePotential;
    result.features.effectiveThreatExposureWeight =
        effectiveWeights.threatExposure;
    result.features.effectiveFragmentationWeight =
        effectiveWeights.fragmentation;

    result.features.totalScore =
        effectiveWeights.population * result.features.populationAdvantage +
        effectiveWeights.humanPotential *
            result.features.humanPotentialAdvantage +
        effectiveWeights.housePotential *
            result.features.housePotentialAdvantage +
        effectiveWeights.threatExposure * result.features.threatAdvantage +
        effectiveWeights.fragmentation *
            result.features.fragmentationAdvantage;

    return result;
}

} // namespace vvw::ngs
