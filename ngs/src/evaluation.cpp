#include "evaluation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
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

struct DistanceField {
    std::vector<int> distance;
    std::vector<Population> nearestSourcePopulation;
};

struct DistanceNode {
    std::size_t index{0};
    int distance{0};
    Population sourcePopulation{0};
};

struct DistanceNodeLater {
    bool operator()(const DistanceNode& left, const DistanceNode& right) const noexcept {
        if (left.distance != right.distance) {
            return left.distance > right.distance;
        }
        return left.sourcePopulation < right.sourcePopulation;
    }
};

// Multi-source shortest-path field over the eight-neighbour grid. For equal
// distances, prefer the strongest source group. This lets evaluation measure
// whether a group has enough strength to exploit a nearby human opportunity,
// rather than blindly rewarding the closest singleton.
DistanceField buildDistanceMap(
    const BoardState& board,
    Species species,
    Population minimumGroupPopulation
) {
    const std::size_t count = board.cells.size();
    DistanceField field;
    field.distance.assign(count, -1);
    field.nearestSourcePopulation.assign(count, 0);

    std::priority_queue<
        DistanceNode,
        std::vector<DistanceNode>,
        DistanceNodeLater
    > frontier;

    for (std::size_t i = 0; i < count; ++i) {
        const Population population = populationFor(board.cells[i], species);
        if (population < minimumGroupPopulation) {
            continue;
        }

        if (field.distance[i] == -1 ||
            population > field.nearestSourcePopulation[i]) {
            field.distance[i] = 0;
            field.nearestSourcePopulation[i] = population;
            frontier.push({i, 0, population});
        }
    }

    while (!frontier.empty()) {
        const DistanceNode current = frontier.top();
        frontier.pop();

        if (field.distance[current.index] != current.distance ||
            field.nearestSourcePopulation[current.index] !=
                current.sourcePopulation) {
            continue;
        }

        const int x = static_cast<int>(current.index %
                                       static_cast<std::size_t>(board.columns));
        const int y = static_cast<int>(current.index /
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
                const int nextDistance = current.distance + 1;

                const bool shorter = field.distance[next] == -1 ||
                                     nextDistance < field.distance[next];
                const bool equallyNearButStronger =
                    nextDistance == field.distance[next] &&
                    current.sourcePopulation >
                        field.nearestSourcePopulation[next];
                if (!shorter && !equallyNearButStronger) {
                    continue;
                }

                field.distance[next] = nextDistance;
                field.nearestSourcePopulation[next] =
                    current.sourcePopulation;
                frontier.push({
                    next, nextDistance, current.sourcePopulation
                });
            }
        }
    }

    return field;
}

double calculateHumanPotential(
    const BoardState& board,
    const DistanceField& distanceForOneHuman,
    const DistanceField& distanceForTwoHumans,
    const DistanceField& distanceForUsefulGroup
) noexcept {
    // A singleton may pursue a one-human cell, and a pair may pursue a
    // two-human cell. For larger clusters, only groups of at least three are
    // considered, and the value is capped by the nearest capable group's
    // population. Consequently a nearby 1-creature fragment cannot outweigh
    // a distant, productive main force.
    double potential = 0.0;

    for (std::size_t i = 0; i < board.cells.size(); ++i) {
        const Population humans = board.cells[i].humans;
        if (humans == 0) {
            continue;
        }

        const DistanceField* field = nullptr;
        if (humans == 1) {
            field = &distanceForOneHuman;
        } else if (humans == 2) {
            field = &distanceForTwoHumans;
        } else {
            field = &distanceForUsefulGroup;
        }

        if (field->distance[i] < 0 ||
            field->nearestSourcePopulation[i] == 0) {
            continue;
        }

        const Population effectiveTarget =
            std::min(humans, field->nearestSourcePopulation[i]);
        const double closeness =
            1.0 / (1.0 + static_cast<double>(field->distance[i]));
        potential += static_cast<double>(effectiveTarget) * closeness;
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
                target.humans, std::move(distances.distance)
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

    // Human attraction is now ability-aware. A singleton can pursue a
    // one-human cell, but it cannot make a 10-human cluster look like an
    // immediately useful target. Groups of at least three provide the
    // general-purpose human potential field; one- and two-human cells use
    // matching lower thresholds.
    const DistanceField ownDistanceForOne =
        buildDistanceMap(board, perspectiveSpecies, 1);
    const DistanceField ownDistanceForTwo =
        buildDistanceMap(board, perspectiveSpecies, 2);
    const DistanceField ownDistanceForUsefulGroup =
        buildDistanceMap(board, perspectiveSpecies, 3);
    const DistanceField opponentDistanceForOne =
        buildDistanceMap(board, opponentSpecies, 1);
    const DistanceField opponentDistanceForTwo =
        buildDistanceMap(board, opponentSpecies, 2);
    const DistanceField opponentDistanceForUsefulGroup =
        buildDistanceMap(board, opponentSpecies, 3);

    result.features.ownHumanPotential = calculateHumanPotential(
        board,
        ownDistanceForOne,
        ownDistanceForTwo,
        ownDistanceForUsefulGroup);
    result.features.opponentHumanPotential = calculateHumanPotential(
        board,
        opponentDistanceForOne,
        opponentDistanceForTwo,
        opponentDistanceForUsefulGroup);
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
