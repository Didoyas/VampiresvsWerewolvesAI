
#pragma once

#include "movement_rules.hpp"

#include <cstdint>
#include <random>

namespace vvw::ngs {

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

struct TransitionOptions {
    // According to the official assignment PDF, when attackers win a random
    // battle against humans, surviving humans can be converted.
    //
    // Set false to test the conflicting earlier team interpretation.
    bool convertHumansAfterRandomBattle{true};
};

// -----------------------------------------------------------------------------
// Transition errors
// -----------------------------------------------------------------------------

enum class TransitionError {
    None,

    InvalidBoard,
    InvalidSpecies,
    InvalidPlan,

    // The destination contains incompatible populations simultaneously.
    UnsupportedMixedOccupancy,

    // Population addition would exceed the Population type's capacity.
    PopulationOverflow
};

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

struct TransitionStats {
    std::uint32_t movesApplied{0};

    std::uint32_t randomBattles{0};
    std::uint32_t randomBattleAttackerWins{0};
    std::uint32_t randomBattleAttackerLosses{0};

    std::uint32_t deterministicCreatureWins{0};
    std::uint32_t deterministicCreatureLosses{0};

    std::uint64_t humansConverted{0};
};

// -----------------------------------------------------------------------------
// Result
// -----------------------------------------------------------------------------

struct TransitionResult {
    TransitionError error{TransitionError::None};

    // If the plan is invalid, this contains the validator's specific error.
    ValidationError validationError{ValidationError::None};

    int moveIndex{-1};

    // Populated only when the transition succeeds.
    BoardState nextState{};

    TransitionStats stats{};

    bool valid() const noexcept {
        return error == TransitionError::None;
    }
};

// -----------------------------------------------------------------------------
// Public functions
// -----------------------------------------------------------------------------

// Returns the probability that the attacking side wins a random battle.
//
// This function implements the probability formula from the assignment.
// Deterministic threshold decisions are handled separately.
double attackerWinProbability(
    Population attackers,
    Population defenders
) noexcept;

// Applies a complete turn plan to a board.
//
// The supplied random engine makes stochastic transitions reproducible
// when the same initial state and seed are used.
TransitionResult applyTurnPlan(
    const BoardState& board,
    const TurnPlan& plan,
    Species actingSpecies,
    std::mt19937& randomEngine,
    TransitionOptions options = {}
);

} // namespace vvw::ngs
