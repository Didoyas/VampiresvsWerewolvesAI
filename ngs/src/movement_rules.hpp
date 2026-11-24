
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

// Movement legality is checked within the current complete plan. The
// assignment rules do not impose a cross-ply source/destination footprint rule.
struct ValidationOptions {
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
