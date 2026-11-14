
#include "movement_rules.hpp"
#include <cstdlib>
#include <vector>

namespace vvw::ngs {
namespace {

// The MOV protocol encodes each movement count in one byte.
constexpr Population kMaxWireMovementCount = 255;

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

    return failure(ValidationError::None);
}

} // namespace vvw::ngs
