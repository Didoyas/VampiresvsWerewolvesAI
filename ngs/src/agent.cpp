#include "agent.hpp"

#include "movement_rules.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace vvw::ngs {
namespace {

constexpr Population kMaxWireMovementCount = 255;

Population populationFor(const Cell& cell, Species species) noexcept {
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


} // namespace

AgentOptions::AgentOptions() {
    // Candidate generation is deliberately capped for 50x50 boards.
    // Four-movement plans are enabled with a small quota to limit branching.
    candidateGenerationOptions.maxMovesPerPlan = 4;
    candidateGenerationOptions.maxSingleMoveCandidates = 32;
    candidateGenerationOptions.maxTwoMoveCandidates = 48;
    candidateGenerationOptions.maxThreeMoveCandidates = 16;
    candidateGenerationOptions.maxFourMoveCandidates = 4;
    candidateGenerationOptions.variantsPerSourceCombination = 2;
    candidateGenerationOptions.maxCombinationAttempts = 1500;

    // Root pre-ranking and response search are both bounded. Tighten these
    // only after profiling the complete client on maximum-size boards.
    // Rank the full bounded portfolio (32 + 48 + 16 + 4 by default).
    // chooseMove() raises this further when custom generation quotas are used.
    opponentSearchOptions.rootRankingOptions.maxCandidatesToEvaluate = 100;
    opponentSearchOptions.rootRankingOptions.simulationsPerCandidate = 1;
    opponentSearchOptions.maxRootCandidates = 4;
    opponentSearchOptions.maxOpponentResponses = 8;
    opponentSearchOptions.maxLikelyOpponentResponses = 4;
    opponentSearchOptions.maxAdversarialOpponentResponses = 8;
    opponentSearchOptions.simulationsPerRootCandidate = 1;

    opponentSearchOptions.opponentGenerationOptions.maxMovesPerPlan = 3;
    opponentSearchOptions.opponentGenerationOptions.maxSingleMoveCandidates = 24;
    opponentSearchOptions.opponentGenerationOptions.maxTwoMoveCandidates = 32;
    opponentSearchOptions.opponentGenerationOptions.maxThreeMoveCandidates = 12;
    opponentSearchOptions.opponentGenerationOptions.variantsPerSourceCombination = 2;
    opponentSearchOptions.opponentGenerationOptions.maxCombinationAttempts = 1000;
}

Agent::Agent(Species ourSpecies, AgentOptions options)
    : ourSpecies_(ourSpecies),
      options_(std::move(options)),
      history_(opponentOf(ourSpecies)) {
}

Species Agent::ourSpecies() const noexcept {
    return ourSpecies_;
}

std::uint32_t Agent::nextTurnNumber() const noexcept {
    return nextTurnNumber_;
}

const OpponentHistory& Agent::history() const noexcept {
    return history_;
}

bool Agent::observeInitialState(const BoardState& initialBoard) {
    if (!initialBoard.hasValidStorage() ||
        !isPlayableSpecies(ourSpecies_) ||
        initialBoard.ourSpecies != ourSpecies_ ||
        initialStateObserved_) {
        return false;
    }

    const HistoryUpdateResult update = history_.recordObservation(
        initialBoard,
        nextHistoryUpdateNumber_
    );

    if (!update.valid()) {
        return false;
    }

    initialStateObserved_ = true;
    if (nextHistoryUpdateNumber_ < std::numeric_limits<std::uint32_t>::max()) {
        ++nextHistoryUpdateNumber_;
    }
    return true;
}

bool Agent::isPlayableSpecies(Species species) const noexcept {
    return species == Species::Vampires || species == Species::Werewolves;
}

bool Agent::validatePlan(
    const BoardState& board,
    const TurnPlan& plan
) const {
    ValidationOptions validationOptions;
    return validateTurnPlan(
        board,
        plan,
        ourSpecies_,
        validationOptions
    ).valid();
}

TurnPlan Agent::makeEmergencyFallback(const BoardState& board) const {
    static constexpr int directions[8][2] = {
        {-1, -1}, {0, -1}, {1, -1},
        {-1,  0},          {1,  0},
        {-1,  1}, {0,  1}, {1,  1}
    };

    const ValidationOptions validationOptions{};

    // This is a safety response, not strategic search.
    // It must still run if the search deadline has expired.
    // Return immediately after finding the first legal move.
    for (int y = 0; y < board.rows; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            const Position source{x, y};

            const Population available =
                populationFor(board.at(source), ourSpecies_);

            if (available == 0) {
                continue;
            }

            const Population moving =
                std::min(available, kMaxWireMovementCount);

            for (const auto& direction : directions) {
                const Position destination{
                    x + direction[0], y + direction[1]
                };

                if (!board.inBounds(destination)) {
                    continue;
                }

                TurnPlan candidate;
                candidate.moves.push_back(
                    Move{source, moving, destination}
                );

                if (validateTurnPlan(
                        board,
                        candidate,
                        ourSpecies_,
                        validationOptions
                    ).valid()) {
                    return candidate;
                }
            }
        }
    }

    return {};
}

void Agent::advanceTurn() noexcept {
    if (nextTurnNumber_ < std::numeric_limits<std::uint32_t>::max()) {
        ++nextTurnNumber_;
    }
}

AgentDecision Agent::chooseMove(const BoardState& board) {
    const auto legacyDeadline =
        std::chrono::steady_clock::now() + options_.searchBudget;
    return chooseMove(board, legacyDeadline);
}

AgentDecision Agent::chooseMove(
    const BoardState& board,
    std::chrono::steady_clock::time_point deadline
) {
    using Clock = std::chrono::steady_clock;
    const auto startedAt = Clock::now();

    AgentDecision decision;
    decision.turnNumber = nextTurnNumber_;
    decision.previousPlyWasUnknown = !board.previousPly.known;
    decision.crossPlyRestrictionsChecked = false;

    const auto finish = [&]() {
        decision.elapsedMicroseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - startedAt
            ).count()
        );
        decision.deadlineExceeded =
            decision.deadlineExceeded || Clock::now() >= deadline;
        advanceTurn();
        return decision;
    };

    if (!board.hasValidStorage() ||
        !isPlayableSpecies(ourSpecies_) ||
        board.ourSpecies != ourSpecies_) {
        decision.method = AgentDecisionMethod::InvalidInput;
        return finish();
    }

    // Establish a legal response before history updates, candidate generation,
    // or opponent search. Keep it available as the safety fallback.
    const TurnPlan emergencyFallback = makeEmergencyFallback(board);

    // The protocol does not expose the previous opponent MOV. Do not infer
    // exact sources/destinations from sparse UPD cell snapshots; record only
    // what was actually observed.
    const std::uint32_t historyUpdateNumber = nextHistoryUpdateNumber_;
    decision.historyObservationNumber = historyUpdateNumber;
    const auto historyUpdateStartedAt = Clock::now();
    const HistoryUpdateResult historyUpdate =
        history_.recordObservation(board, historyUpdateNumber);
    decision.historyUpdateMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - historyUpdateStartedAt
        ).count()
    );
    decision.historyUpdateAttempted = true;
    decision.historyUpdateError = historyUpdate.error;
    decision.historyUpdated = historyUpdate.valid();
    decision.historySnapshotsRetained = history_.size();
    decision.confirmedMovementObservations =
        history_.confirmedMovements().size();
    if (decision.historyUpdated &&
        nextHistoryUpdateNumber_ < std::numeric_limits<std::uint32_t>::max()) {
        ++nextHistoryUpdateNumber_;
    }

    // History recording is a bounded but non-interruptible operation. Do not
    // start candidate generation if it consumed the remaining turn budget.
    if (Clock::now() >= deadline) {
        decision.deadlineExceeded = true;
        decision.plan = emergencyFallback;
        decision.method = decision.plan.moves.empty()
            ? AgentDecisionMethod::NoLegalMove
            : AgentDecisionMethod::EmergencyFallback;
        return finish();
    }

    MultiMoveCandidateOptions generationOptions =
        options_.candidateGenerationOptions;

    const auto candidateGenerationStartedAt = Clock::now();
    const MultiMoveCandidateResult generated =
        generateMultiMoveCandidates(
            board,
            ourSpecies_,
            generationOptions,
            deadline
        );
    decision.candidateGenerationMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - candidateGenerationStartedAt
        ).count()
    );

    decision.deadlineExceeded =
        generated.timedOut || Clock::now() >= deadline;

    if (generated.valid()) {
        decision.candidatesGenerated = generated.candidates.size();
        decision.candidateGenerationStats = generated.stats;
    }

    // Set both transition validation and opponent candidate generation to
    // match the information actually available in BoardState.
    OpponentSearchOptions searchOptions = options_.opponentSearchOptions;
    // Candidate generation is already bounded by the configured per-class
    // quotas. Rank the entire generated portfolio so a strong move cannot be
    // discarded by a second, position-based sampling pass in candidate_ranker.
    searchOptions.rootRankingOptions.maxCandidatesToEvaluate = std::max(
        searchOptions.rootRankingOptions.maxCandidatesToEvaluate,
        generated.candidates.size()
    );
    // Do not carry the unsupported cross-ply footprint restriction into
    // simulated root or opponent transitions. Same-ply conflicts remain
    // enforced by validateTurnPlan().

    bool selectedBySearch = false;
    bool selectedByRootRankingFallback = false;
    if (generated.valid() && !generated.timedOut &&
        !generated.candidates.empty() && Clock::now() < deadline) {
        OpponentSearchResult search;
        if (decision.historyUpdated) {
            search = searchTurnCandidates(
                board,
                generated.candidates,
                ourSpecies_,
                history_,
                historyUpdateNumber,
                searchOptions,
                deadline
            );
            decision.opponentSearchStats = search.stats;
            decision.historyAwareSearchUsed =
                search.stats.weightedExpectedEvaluations > 0;
        } else {
            search = searchTurnCandidates(
                board,
                generated.candidates,
                ourSpecies_,
                searchOptions,
                deadline
            );
            decision.opponentSearchStats = search.stats;
        }

        decision.deadlineExceeded =
            decision.deadlineExceeded || search.timedOut ||
            Clock::now() >= deadline;
        decision.rootCandidatesEvaluated = search.stats.rootCandidatesEvaluated;
        decision.opponentResponsesEvaluated = search.stats.opponentResponsesEvaluated;

        // A completed deeper search is preferred. A timeout during deeper
        // search may still leave completed root candidates in this result.
        if (search.valid() && search.hasBestCandidate &&
            search.bestCandidateIndex < generated.candidates.size()) {
            decision.plan = generated.candidates[search.bestCandidateIndex];
            if (Clock::now() >= deadline) {
                // Candidate generation already validated every returned plan.
                decision.deadlineExceeded = true;
                selectedBySearch = true;
            } else {
                selectedBySearch = validatePlan(board, decision.plan);
            }
        }

        // If root ranking completed but opponent-response search timed out,
        // retain its best fully ranked root candidate instead of discarding
        // all useful work and reverting immediately to the emergency move.
        if (!selectedBySearch && search.hasRootRankingFallbackCandidate &&
            search.rootRankingFallbackCandidateIndex <
                generated.candidates.size()) {
            decision.plan = generated.candidates[
                search.rootRankingFallbackCandidateIndex
            ];
            if (Clock::now() >= deadline) {
                // This plan also comes from the already-validated generator.
                decision.deadlineExceeded = true;
                selectedByRootRankingFallback = true;
            } else {
                selectedByRootRankingFallback = validatePlan(board, decision.plan);
            }
        }
    }

    if (selectedBySearch) {
        decision.method = AgentDecisionMethod::Search;
        return finish();
    }

    if (selectedByRootRankingFallback) {
        decision.method = AgentDecisionMethod::CandidateFallback;
        return finish();
    }

    // Every plan emitted by the candidate generator has already passed the
    // movement validator. When time remains, revalidate before selecting it;
    // once the deadline is reached, use the generator's validated first plan.
    if (generated.valid() && !generated.candidates.empty()) {
        for (const TurnPlan& candidate : generated.candidates) {
            if (Clock::now() >= deadline) {
                decision.deadlineExceeded = true;
                break;
            }
            if (validatePlan(board, candidate)) {
                decision.plan = candidate;
                decision.method = AgentDecisionMethod::CandidateFallback;
                return finish();
            }
        }

        decision.plan = generated.candidates.front();
        decision.method = AgentDecisionMethod::CandidateFallback;
        return finish();
    }

    decision.plan = emergencyFallback;
    decision.method = decision.plan.moves.empty()
        ? AgentDecisionMethod::NoLegalMove
        : AgentDecisionMethod::EmergencyFallback;

    return finish();
}

} // namespace vvw::ngs
