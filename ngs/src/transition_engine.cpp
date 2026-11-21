
#include "transition_engine.hpp"
#include "types.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace vvw::ngs {

namespace {

// -----------------------------------------------------------------------------
// Basic helpers
// -----------------------------------------------------------------------------

Population& populationFor(Cell& cell, Species species) noexcept {
    if (species == Species::Vampires) {
        return cell.vampires;
    }

    return cell.werewolves;
}

bool checkedAdd(
    Population a,
    Population b,
    Population& result
) noexcept {
    const Population maxValue =
        std::numeric_limits<Population>::max();

    if (b > maxValue - a) {
        return false;
    }

    result = static_cast<Population>(a + b);
    return true;
}

// Uses integer arithmetic so that the 1.5 threshold is exact.
//
// attackers >= 1.5 * defenders
// is equivalent to
// 2 * attackers >= 3 * defenders.
bool attackersMeetOneAndHalfThreshold(
    Population attackers,
    Population defenders
) noexcept {
    const std::uint64_t lhs =
        2ULL * static_cast<std::uint64_t>(attackers);

    const std::uint64_t rhs =
        3ULL * static_cast<std::uint64_t>(defenders);

    return lhs >= rhs;
}

bool defendersMeetOneAndHalfThreshold(
    Population attackers,
    Population defenders
) noexcept {
    const std::uint64_t lhs =
        2ULL * static_cast<std::uint64_t>(defenders);

    const std::uint64_t rhs =
        3ULL * static_cast<std::uint64_t>(attackers);

    return lhs >= rhs;
}

// -----------------------------------------------------------------------------
// Probability and random survival
// -----------------------------------------------------------------------------

Population sampleSurvivors(
    Population population,
    double survivalProbability,
    std::mt19937& randomEngine
) {
    if (population == 0) {
        return 0;
    }

    survivalProbability =
        std::clamp(survivalProbability, 0.0, 1.0);

    // The assignment describes each individual creature as having
    // a probability of survival.
    //
    // This binomial implementation assumes independent survival events.
    // Verify that assumption against the official server.
    std::binomial_distribution<unsigned int> distribution(
        static_cast<unsigned int>(population),
        survivalProbability
    );

    return static_cast<Population>(distribution(randomEngine));
}

bool sampleAttackerVictory(
    double probability,
    std::mt19937& randomEngine
) {
    std::bernoulli_distribution distribution(
        std::clamp(probability, 0.0, 1.0)
    );

    return distribution(randomEngine);
}

// -----------------------------------------------------------------------------
// Statistics helpers
// -----------------------------------------------------------------------------

void recordHumanConversion(
    TransitionStats& stats,
    Population converted
) noexcept {
    stats.humansConverted +=
        static_cast<std::uint64_t>(converted);
}

// -----------------------------------------------------------------------------
// Human encounter
// -----------------------------------------------------------------------------

TransitionError resolveHumanEncounter(
    Cell& destination,
    Species actingSpecies,
    Population attackers,
    const TransitionOptions& options,
    std::mt19937& randomEngine,
    TransitionStats& stats
) {
    Population& ownPopulation =
        populationFor(destination, actingSpecies);

    const Population humanPopulation = destination.humans;

    if (humanPopulation == 0) {
        return TransitionError::None;
    }

    // Normal conversion:
    // attackers must be at least as numerous as humans.
    if (attackers >= humanPopulation) {
        Population updatedOwnPopulation{};

        if (!checkedAdd(
                attackers,
                humanPopulation,
                updatedOwnPopulation)) {
            return TransitionError::PopulationOverflow;
        }

        ownPopulation = updatedOwnPopulation;
        destination.humans = 0;

        recordHumanConversion(stats, humanPopulation);

        return TransitionError::None;
    }

    // Otherwise, a random battle occurs.
    const double probability =
        attackerWinProbability(attackers, humanPopulation);

    ++stats.randomBattles;

    const bool attackersWin =
        sampleAttackerVictory(probability, randomEngine);

    if (attackersWin) {
        ++stats.randomBattleAttackerWins;

        const Population survivingAttackers =
            sampleSurvivors(
                attackers,
                probability,
                randomEngine
            );

        const Population survivingHumans =
            sampleSurvivors(
                humanPopulation,
                probability,
                randomEngine
            );

        ownPopulation = survivingAttackers;

        if (options.convertHumansAfterRandomBattle) {
            Population updatedOwnPopulation{};

            if (!checkedAdd(
                    ownPopulation,
                    survivingHumans,
                    updatedOwnPopulation)) {
                return TransitionError::PopulationOverflow;
            }

            ownPopulation = updatedOwnPopulation;
            destination.humans = 0;

            recordHumanConversion(stats, survivingHumans);
        } else {
            // Alternative interpretation: surviving humans remain human.
            destination.humans = survivingHumans;
        }
    } else {
        ++stats.randomBattleAttackerLosses;

        // The attackers lose.
        ownPopulation = 0;

        destination.humans =
            sampleSurvivors(
                humanPopulation,
                1.0 - probability,
                randomEngine
            );
    }

    return TransitionError::None;
}

// -----------------------------------------------------------------------------
// Creature encounter
// -----------------------------------------------------------------------------

TransitionError resolveCreatureEncounter(
    Cell& destination,
    Species actingSpecies,
    Population attackers,
    const TransitionOptions& options,
    std::mt19937& randomEngine,
    TransitionStats& stats
) {
    (void)options;

    const Species defendingSpecies =
        opponentOf(actingSpecies);

    Population& ownPopulation =
        populationFor(destination, actingSpecies);

    Population& enemyPopulation =
        populationFor(destination, defendingSpecies);

    const Population defenders = enemyPopulation;

    // Deterministic attacker victory:
    // attackers >= 1.5 * defenders.
    if (attackersMeetOneAndHalfThreshold(attackers, defenders)) {
        ownPopulation = attackers;
        enemyPopulation = 0;

        ++stats.deterministicCreatureWins;

        return TransitionError::None;
    }

    // Deterministic defender victory:
    // defenders >= 1.5 * attackers.
    if (defendersMeetOneAndHalfThreshold(attackers, defenders)) {
        ownPopulation = 0;

        // Defenders retain their original population in this
        // deterministic outcome.
        ++stats.deterministicCreatureLosses;

        return TransitionError::None;
    }

    // Neither side reaches the deterministic threshold.
    // Resolve a random battle.
    const double probability =
        attackerWinProbability(attackers, defenders);

    ++stats.randomBattles;

    const bool attackersWin =
        sampleAttackerVictory(probability, randomEngine);

    if (attackersWin) {
        ++stats.randomBattleAttackerWins;

        ownPopulation =
            sampleSurvivors(
                attackers,
                probability,
                randomEngine
            );

        enemyPopulation = 0;
    } else {
        ++stats.randomBattleAttackerLosses;

        ownPopulation = 0;

        enemyPopulation =
            sampleSurvivors(
                defenders,
                1.0 - probability,
                randomEngine
            );
    }

    return TransitionError::None;
}

// -----------------------------------------------------------------------------
// Resolve one aggregated destination arrival
// -----------------------------------------------------------------------------

TransitionError resolveArrival(
    Cell& destination,
    Species actingSpecies,
    Population incomingPopulation,
    const TransitionOptions& options,
    std::mt19937& randomEngine,
    TransitionStats& stats
) {
    const Species defendingSpecies =
        opponentOf(actingSpecies);

    Population& ownPopulation =
        populationFor(destination, actingSpecies);

    const Population enemyPopulation =
        populationFor(destination, defendingSpecies);

    // This initial engine supports:
    //   - empty cells
    //   - friendly groups
    //   - human groups
    //   - enemy groups
    //
    // It does not guess how the server handles a cell containing
    // both enemy creatures and humans, or both creature species.
    if (enemyPopulation > 0 &&
        (destination.humans > 0 || ownPopulation > 0)) {
        return TransitionError::UnsupportedMixedOccupancy;
    }

    // Case 1: destination contains enemy creatures.
    if (enemyPopulation > 0) {
        return resolveCreatureEncounter(
            destination,
            actingSpecies,
            incomingPopulation,
            options,
            randomEngine,
            stats
        );
    }

    // Case 2: destination contains humans.
    //
    // If friendly creatures are already present, they combine with
    // the arriving group before the human encounter is resolved.
    if (destination.humans > 0) {
        Population combinedAttackers{};

        if (!checkedAdd(
                ownPopulation,
                incomingPopulation,
                combinedAttackers)) {
            return TransitionError::PopulationOverflow;
        }

        return resolveHumanEncounter(
            destination,
            actingSpecies,
            combinedAttackers,
            options,
            randomEngine,
            stats
        );
    }

    // Case 3: empty or friendly destination.
    Population updatedOwnPopulation{};

    if (!checkedAdd(
            ownPopulation,
            incomingPopulation,
            updatedOwnPopulation)) {
        return TransitionError::PopulationOverflow;
    }

    ownPopulation = updatedOwnPopulation;

    return TransitionError::None;
}

// -----------------------------------------------------------------------------
// Error construction
// -----------------------------------------------------------------------------

TransitionResult makeFailure(
    TransitionError error,
    ValidationError validationError = ValidationError::None,
    int moveIndex = -1
) {
    TransitionResult result;

    result.error = error;
    result.validationError = validationError;
    result.moveIndex = moveIndex;

    return result;
}

// -----------------------------------------------------------------------------
// Footprint helper
// -----------------------------------------------------------------------------

void addUniquePosition(
    std::vector<Position>& positions,
    Position position
) {
    if (std::find(
            positions.begin(),
            positions.end(),
            position
        ) == positions.end()) {
        positions.push_back(position);
    }
}

void incrementPliesUsed(std::uint16_t& pliesUsed) noexcept {
    if (pliesUsed < GameHorizon::kMaxPliesPerSide) {
        ++pliesUsed;
    }
}

void advanceGameHorizon(
    BoardState& state,
    Species actingSpecies
) noexcept {
    GameHorizon& horizon = state.horizon;
    if (!horizon.hasValidValues()) {
        return;
    }

    if (actingSpecies == state.ourSpecies) {
        incrementPliesUsed(horizon.ourPliesUsed);
    } else if (actingSpecies == opponentOf(state.ourSpecies)) {
        incrementPliesUsed(horizon.opponentPliesUsed);
    }
    // elapsedSeconds is wall-clock observation data. Simulated plies do not
    // consume real game time, so it is deliberately left unchanged.
}

} // namespace


// -----------------------------------------------------------------------------
// Public probability function
// -----------------------------------------------------------------------------

double attackerWinProbability(
    Population attackers,
    Population defenders
) noexcept {
    if (attackers == 0) {
        return 0.0;
    }

    if (defenders == 0) {
        return 1.0;
    }

    double probability{};

    if (attackers == defenders) {
        probability = 0.5;
    } else if (attackers < defenders) {
        probability =
            static_cast<double>(attackers) /
            (2.0 * static_cast<double>(defenders));
    } else {
        probability =
            static_cast<double>(attackers) /
            static_cast<double>(defenders) - 0.5;
    }

    return std::clamp(probability, 0.0, 1.0);
}


// -----------------------------------------------------------------------------
// Apply complete turn plan
// -----------------------------------------------------------------------------

TransitionResult applyTurnPlan(
    const BoardState& board,
    const TurnPlan& plan,
    Species actingSpecies,
    std::mt19937& randomEngine,
    TransitionOptions options
) {
    // 1. Validate the board.
    if (!board.hasValidStorage()) {
        return makeFailure(TransitionError::InvalidBoard);
    }

    if (actingSpecies != Species::Vampires &&
        actingSpecies != Species::Werewolves) {
        return makeFailure(TransitionError::InvalidSpecies);
    }

    // 2. Validate the entire movement plan before changing the state.
    ValidationOptions validationOptions;
    validationOptions.requirePreviousPlyFootprint =
        options.requirePreviousPlyFootprint;
    validationOptions.convertHumansAfterRandomBattle =
        options.convertHumansAfterRandomBattle;

    const ValidationResult validation =
        validateTurnPlan(
            board,
            plan,
            actingSpecies,
            validationOptions
        );

    if (!validation.valid()) {
        return makeFailure(
            TransitionError::InvalidPlan,
            validation.error,
            validation.moveIndex
        );
    }

    // 3. Work on a copy. The original board is never modified.
    BoardState nextState = board;
    TransitionStats stats;

    const std::size_t cellCount = board.cells.size();

    // Outgoing population from each source cell.
    std::vector<Population> outgoing(cellCount, 0);

    // Incoming population for each destination cell.
    //
    // Important assumption:
    // Multiple movements arriving at one destination are aggregated
    // and resolved as one encounter.
    std::vector<Population> incoming(cellCount, 0);

    // 4. Aggregate all movements.
    for (const Move& move : plan.moves) {
        const std::size_t sourceIndex =
            board.index(move.source);

        const std::size_t destinationIndex =
            board.index(move.destination);

        Population updatedOutgoing{};
        Population updatedIncoming{};

        if (!checkedAdd(
                outgoing[sourceIndex],
                move.count,
                updatedOutgoing) ||
            !checkedAdd(
                incoming[destinationIndex],
                move.count,
                updatedIncoming)) {
            return makeFailure(
                TransitionError::PopulationOverflow
            );
        }

        outgoing[sourceIndex] = updatedOutgoing;
        incoming[destinationIndex] = updatedIncoming;
    }

    // 5. Deduct all outgoing creatures from their source cells.
    //
    // This is done before resolving any destination so that the
    // movement plan is treated as a simultaneous departure phase.
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (outgoing[i] == 0) {
            continue;
        }

        Population& sourcePopulation =
            populationFor(
                nextState.cells[i],
                actingSpecies
            );

        // The validator has already checked that this subtraction
        // is legal.
        sourcePopulation =
            static_cast<Population>(
                sourcePopulation - outgoing[i]
            );
    }

    // 6. Resolve all destination arrivals.
    //
    // Every destination is processed once, using the aggregated
    // incoming population for that cell.
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (incoming[i] == 0) {
            continue;
        }

        Cell& destination = nextState.cells[i];

        const TransitionError arrivalError =
            resolveArrival(
                destination,
                actingSpecies,
                incoming[i],
                options,
                randomEngine,
                stats
            );

        if (arrivalError != TransitionError::None) {
            return makeFailure(arrivalError);
        }
    }

    // 7. Record the current ply's footprint.
    //
    // The next player will need this information to validate
    // the cross-ply source/destination restrictions.
    nextState.previousPly.known = true;
    nextState.previousPly.sources.clear();
    nextState.previousPly.destinations.clear();

    for (const Move& move : plan.moves) {
        addUniquePosition(
            nextState.previousPly.sources,
            move.source
        );

        addUniquePosition(
            nextState.previousPly.destinations,
            move.destination
        );
    }

    // 8. Advance the simulated side-to-move count only after the full turn
    // has resolved successfully. The counter is relative to board.ourSpecies.
    advanceGameHorizon(nextState, actingSpecies);

    // 9. Construct successful result.
    TransitionResult result;
    result.nextState = std::move(nextState);
    result.stats = stats;

    result.stats.movesApplied =
        static_cast<std::uint32_t>(plan.moves.size());

    return result;
}

} // namespace vvw::ngs
