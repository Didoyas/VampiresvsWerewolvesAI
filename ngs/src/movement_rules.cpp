
#include "movement_rules.hpp"
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <vector>

namespace vvw::ngs {
namespace {

// MOV movement counts and board population fields are encoded in one byte.
constexpr std::uint64_t kMaxWirePopulation = 255;
constexpr Population kMaxWireMovementCount =
    static_cast<Population>(kMaxWirePopulation);

bool isKnownSpecies(Species species) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

Population populationOf(const Cell& cell, Species species) noexcept {
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

bool areAdjacent(Position a, Position b) noexcept {
    const int dx = std::abs(a.x - b.x);
    const int dy = std::abs(a.y - b.y);

    // Eight-neighbour movement: horizontal, vertical or diagonal.
    // Staying in the same cell is not a movement.
    return (dx <= 1 && dy <= 1 && (dx + dy) > 0);
}

bool containsPosition(
    const std::vector<Position>& positions,
    Position target
) noexcept {
    for (const Position& position : positions) {
        if (position == target) {
            return true;
        }
    }

    return false;
}

ValidationResult failure(
    ValidationError error,
    int moveIndex = -1
) noexcept {
    return {error, moveIndex};
}

} // namespace


ValidationResult validateTurnPlan(
    const BoardState& board,
    const TurnPlan& plan,
    Species actingSpecies,
    ValidationOptions options
) {
    // 1. Validate board storage and dimensions.
    if (!board.hasValidStorage()) {
        return failure(ValidationError::InvalidBoard);
    }

    // 2. The validator must know whose movement it is checking.
    if (!isKnownSpecies(actingSpecies)) {
        return failure(ValidationError::UnknownSpecies);
    }

    // 3. A complete plan must contain at least one movement.
    if (plan.moves.empty()) {
        return failure(ValidationError::EmptyPlan);
    }

    // 4. Cross-ply restrictions require the previous ply's footprint.
    if (options.requirePreviousPlyFootprint &&
        !board.previousPly.known) {
        return failure(ValidationError::MissingPreviousPlyFootprint);
    }

    // Accumulate outgoing populations for every source cell.
    // This catches multiple splits that collectively exceed the group size.
    std::vector<Population> outgoingByCell(board.cells.size(), 0);

    // Aggregate arrivals per destination exactly as applyTurnPlan does.
    // uint64_t keeps the validation sum wider than the population field.
    std::vector<std::uint64_t> incomingByCell(board.cells.size(), 0);
    std::vector<int> firstIncomingMoveIndex(board.cells.size(), -1);

    // Record all sources and destinations in this plan.
    std::vector<Position> currentSources;
    std::vector<Position> currentDestinations;

    currentSources.reserve(plan.moves.size());
    currentDestinations.reserve(plan.moves.size());

    // 5. Validate every individual movement.
    for (std::size_t i = 0; i < plan.moves.size(); ++i) {
        const Move& move = plan.moves[i];
        const int moveIndex = static_cast<int>(i);

        // Both coordinates must be inside the board.
        if (!board.inBounds(move.source) ||
            !board.inBounds(move.destination)) {
            return failure(
                ValidationError::PositionOutOfBounds,
                moveIndex
            );
        }

        // A movement must go to one of the eight neighbouring cells.
        if (!areAdjacent(move.source, move.destination)) {
            return failure(
                ValidationError::NotAdjacent,
                moveIndex
            );
        }

        // Movement count must be positive.
        if (move.count == 0) {
            return failure(
                ValidationError::ZeroMovement,
                moveIndex
            );
        }

        // MOV has a one-byte population field.
        if (move.count > kMaxWireMovementCount) {
            return failure(
                ValidationError::MovementExceedsWireLimit,
                moveIndex
            );
        }

        const Cell& sourceCell = board.at(move.source);

        // The source must contain creatures belonging to the
        // species whose plan is currently being validated.
        const Population available =
            populationOf(sourceCell, actingSpecies);

        if (available == 0) {
            return failure(
                ValidationError::SourceDoesNotContainOurSpecies,
                moveIndex
            );
        }

        const std::size_t sourceIndex = board.index(move.source);

        // Check cumulative outgoing population.
        // Subtraction avoids unsigned integer overflow.
        if (outgoingByCell[sourceIndex] > available ||
            move.count > available - outgoingByCell[sourceIndex]) {
            return failure(
                ValidationError::InsufficientSourcePopulation,
                moveIndex
            );
        }

        outgoingByCell[sourceIndex] += move.count;

        const std::size_t destinationIndex =
            board.index(move.destination);

        // The transition engine aggregates all arrivals at a destination.
        // Preserve the full total because it is also used to evaluate the
        // deterministic combat threshold against an enemy population. Saturate
        // only at uint64_t's limit to make the addition itself overflow-safe.
        const std::uint64_t incomingCount =
            static_cast<std::uint64_t>(move.count);
        std::uint64_t& destinationIncoming =
            incomingByCell[destinationIndex];
        const std::uint64_t maxIncoming =
            std::numeric_limits<std::uint64_t>::max();

        if (incomingCount > maxIncoming - destinationIncoming) {
            destinationIncoming = maxIncoming;
        } else {
            destinationIncoming += incomingCount;
        }
        if (firstIncomingMoveIndex[destinationIndex] == -1) {
            firstIncomingMoveIndex[destinationIndex] = moveIndex;
        }

        // 6. Check restrictions against the previous ply.
        if (board.previousPly.known) {
            // A previous destination cannot become a current source.
            if (board.previousPly.wasDestination(move.source)) {
                return failure(
                    ValidationError::PreviousDestinationUsedAsSource,
                    moveIndex
                );
            }

            // A previous source cannot become a current destination.
            if (board.previousPly.wasSource(move.destination)) {
                return failure(
                    ValidationError::PreviousSourceUsedAsDestination,
                    moveIndex
                );
            }
        }

        // Remember positions for same-ply validation.
        currentSources.push_back(move.source);
        currentDestinations.push_back(move.destination);
    }

    // 7. Check that no cell is both a source and destination
    // in the same ply.
    for (std::size_t i = 0; i < currentSources.size(); ++i) {
        if (containsPosition(
                currentDestinations,
                currentSources[i])) {
            return failure(
                ValidationError::SamePlySourceDestinationConflict,
                static_cast<int>(i)
            );
        }
    }

    // 8. Check the maximum possible resulting friendly population at each
    // destination. The transition engine aggregates arrivals before resolving
    // the encounter, so validation must use the same aggregate.
    //
    // For an empty/friendly destination, existing friendly creatures and all
    // arrivals combine. For a human destination, account for conversion when
    // it is deterministic or enabled after a random attacker victory. For an
    // enemy destination, the attacking population can become the resulting
    // friendly population if the attackers win.
    const Species defendingSpecies = opponentOf(actingSpecies);

    for (std::size_t i = 0; i < incomingByCell.size(); ++i) {
        if (incomingByCell[i] == 0) {
            continue;
        }

        const Cell& destination = board.cells[i];
        const std::uint64_t incomingPopulation = incomingByCell[i];

        const Population existingEnemyPopulation =
            populationOf(destination, defendingSpecies);

        if (existingEnemyPopulation > 0) {
            // A deterministic defender victory leaves no friendly creatures
            // in the destination, so even an aggregate of more than 255
            // attackers cannot overflow the resulting friendly population.
            // Otherwise, an attacker victory (deterministic or random) can
            // leave as many as all incoming attackers alive.
            const std::uint64_t defenders =
                static_cast<std::uint64_t>(existingEnemyPopulation);
            // Equivalent to 2 * defenders >= 3 * attackers, but avoids
            // overflowing uint64_t when incomingPopulation is saturated.
            const bool defendersWinDeterministically =
                incomingPopulation <= (2ULL * defenders) / 3ULL;

            if (!defendersWinDeterministically &&
                incomingPopulation > kMaxWirePopulation) {
                return failure(
                    ValidationError::DestinationPopulationExceedsWireLimit,
                    firstIncomingMoveIndex[i]
                );
            }

            continue;
        }

        // With no enemy group, all arriving friendly creatures combine with
        // any existing friendly population.
        std::uint64_t maximumResultingOwnPopulation = incomingPopulation;

        if (maximumResultingOwnPopulation > kMaxWirePopulation) {
            return failure(
                ValidationError::DestinationPopulationExceedsWireLimit,
                firstIncomingMoveIndex[i]
            );
        }

        const std::uint64_t existingOwnPopulation =
            static_cast<std::uint64_t>(
                populationOf(destination, actingSpecies));

        if (existingOwnPopulation >
            kMaxWirePopulation - maximumResultingOwnPopulation) {
            return failure(
                ValidationError::DestinationPopulationExceedsWireLimit,
                firstIncomingMoveIndex[i]
            );
        }

        maximumResultingOwnPopulation += existingOwnPopulation;

        if (destination.humans > 0) {
            // Conversion is unavoidable when attackers are at least as
            // numerous as humans. Otherwise, it remains possible after a
            // random attacker victory when that transition option is on.
            const bool humansCanBeConverted =
                maximumResultingOwnPopulation >= destination.humans ||
                options.convertHumansAfterRandomBattle;

            if (humansCanBeConverted) {
                const std::uint64_t humanPopulation =
                    static_cast<std::uint64_t>(destination.humans);

                if (humanPopulation >
                    kMaxWirePopulation - maximumResultingOwnPopulation) {
                    return failure(
                        ValidationError::DestinationPopulationExceedsWireLimit,
                        firstIncomingMoveIndex[i]
                    );
                }
            }
        }
    }

    return failure(ValidationError::None);
}

} // namespace vvw::ngs
