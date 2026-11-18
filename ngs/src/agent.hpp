#pragma once

#include "types.hpp"
#include "multi_move_candidate_generator.hpp"
#include "opponent_history.hpp"
#include "opponent_search.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace vvw::ngs {

// Conservative initial limits for the integrated agent. These are workload
// caps, not a hard real-time guarantee: Stage 7's search API is synchronous
// and does not yet accept an interrupt/deadline callback.
struct AgentOptions {
    MultiMoveCandidateOptions candidateGenerationOptions{};
    OpponentSearchOptions opponentSearchOptions{};
    std::chrono::milliseconds searchBudget{1200};

    AgentOptions();
};

enum class AgentDecisionMethod {
    Search,
    CandidateFallback,
    EmergencyFallback,
    NoLegalMove,
    InvalidInput
};

struct AgentDecision {
    TurnPlan plan{};
    AgentDecisionMethod method{AgentDecisionMethod::NoLegalMove};

    std::uint32_t turnNumber{0};
    std::size_t candidatesGenerated{0};
    std::size_t rootCandidatesEvaluated{0};
    std::size_t opponentResponsesEvaluated{0};

    bool historyUpdated{false};
    bool historyAwareSearchUsed{false};
    bool previousPlyWasUnknown{false};
    bool crossPlyRestrictionsChecked{false};

    std::uint64_t elapsedMicroseconds{0};

    bool hasMove() const noexcept {
        return !plan.moves.empty();
    }
};

// Per-game decision controller. Construct one Agent after SET/HUM/HME/MAP;
// keep it alive for that game and discard it after END. It owns opponent
// history and monotonically numbers each UPD decision.
class Agent {
public:
    explicit Agent(
        Species ourSpecies,
        AgentOptions options = {}
    );

    // Seed the per-game history with the complete MAP snapshot before the
    // first UPD. Safe to call once after SET/HUM/HME/MAP.
    bool observeInitialState(const BoardState& initialBoard);

    // Compatibility entry point for callers that do not provide a deadline.
    // The configured searchBudget is used only for this legacy overload.
    AgentDecision chooseMove(const BoardState& board);

    // Uses the caller's absolute steady-clock deadline. It is never reset
    // inside the Agent.
    AgentDecision chooseMove(
        const BoardState& board,
        std::chrono::steady_clock::time_point deadline
    );

    Species ourSpecies() const noexcept;
    std::uint32_t nextTurnNumber() const noexcept;
    const OpponentHistory& history() const noexcept;

private:
    Species ourSpecies_{Species::Unknown};
    AgentOptions options_{};
    OpponentHistory history_;
    std::uint32_t nextTurnNumber_{1};
    std::uint32_t nextHistoryUpdateNumber_{1};
    bool initialStateObserved_{false};

    bool isPlayableSpecies(Species species) const noexcept;
    bool validatePlan(
        const BoardState& board,
        const TurnPlan& plan
    ) const;
    TurnPlan makeEmergencyFallback(
        const BoardState& board,
        std::chrono::steady_clock::time_point deadline
    ) const;
    void advanceTurn() noexcept;
};

} // namespace vvw::ngs
