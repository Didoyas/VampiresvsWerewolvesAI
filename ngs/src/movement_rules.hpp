
#pragma once
#include "types.hpp"

namespace vvw::ngs {

// The validator reports the first rule that a plan violates.
enum class ValidationError {
    None,
    InvalidBoard,
    UnknownSpecies,
    EmptyPlan,
    PositionOutOfBounds,
    NotAdjacent,
    SourceDoesNotContainOurSpecies,
    ZeroMovement,
    MovementExceedsWireLimit,
    InsufficientSourcePopulation,
    SamePlySourceDestinationConflict,
    MissingPreviousPlyFootprint,
    PreviousDestinationUsedAsSource,
    PreviousSourceUsedAsDestination,
    DestinationPopulationExceedsWireLimit
};

struct ValidationResult {
    ValidationError error{ValidationError::None};

    // Index of the offending movement in TurnPlan::moves.
    // It is -1 for errors that concern the plan as a whole.
    int moveIndex{-1};

    bool valid() const noexcept {
        return error == ValidationError::None;
    }
};

// Set requirePreviousPlyFootprint to false only in contexts where
// cross-ply validation is intentionally unavailable, such as isolated tests.
struct ValidationOptions {
    bool requirePreviousPlyFootprint{true};

    // Must match TransitionOptions when validating a simulated transition.
    // The default matches the production transition setting.
    bool convertHumansAfterRandomBattle{true};
};

// Validate a complete movement plan for the specified species.
ValidationResult validateTurnPlan(
    const BoardState& board,
    const TurnPlan& plan,
    Species actingSpecies,
    ValidationOptions options = {}
);

} // namespace vvw::ngs
