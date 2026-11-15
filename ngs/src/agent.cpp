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

int destinationPreference(
    const BoardState& board,
    Position source,
    Position destination,
    Species species,
    Population moving
) noexcept {
    const Cell& target = board.cells[board.index(destination)];
    const Population enemy = populationFor(target, opponentOf(species));
    const Population own = populationFor(target, species);

    if (target.humans > 0) {
        // Prefer a conversion we can plausibly complete; an under-strength
        // human encounter is retained only as a last-resort legal action.
        return moving >= target.humans
            ? 100000 + static_cast<int>(std::min<Population>(target.humans, 10000))
            : -10000;
    }

    if (enemy > 0) {
        // Respect the game's 1.5:1 deterministic combat threshold.
        if (static_cast<std::uint64_t>(moving) * 2 >=
            static_cast<std::uint64_t>(enemy) * 3) {
            return 80000 + static_cast<int>(std::min<Population>(enemy, 10000));
        }
        return -20000;
    }

    if (own > 0) {
        return 20000 + static_cast<int>(std::min<Population>(own, 10000));
    }

    // A house is a strategic target even when it currently has no humans.
    if (target.isHouse) {
        return 15000;
    }

    (void)source;
    return 1000;
}

} // namespace

AgentOptions::AgentOptions() {
    // Candidate generation is deliberately capped for 50x50 boards.
    candidateGenerationOptions.maxMovesPerPlan = 3;
    candidateGenerationOptions.maxSingleMoveCandidates = 32;
    candidateGenerationOptions.maxTwoMoveCandidates = 48;
    candidateGenerationOptions.maxThreeMoveCandidates = 16;
    candidateGenerationOptions.variantsPerSourceCombination = 2;
    candidateGenerationOptions.maxCombinationAttempts = 1500;

    // Root pre-ranking and response search are both bounded. Tighten these
    // only after profiling the complete client on maximum-size boards.
    opponentSearchOptions.rootRankingOptions.maxCandidatesToEvaluate = 24;
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
    // The wire protocol sends cell values, not the opponent's last MOV. The
    // caller may provide a reconstructed footprint in future; until then we
    // enforce every within-ply rule and all cross-ply rules when known.
    validationOptions.requirePreviousPlyFootprint = board.previousPly.known;

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

    TurnPlan bestPlan;
    TurnPlan firstLegalPlan;
    int bestScore = std::numeric_limits<int>::min();
    const ValidationOptions validationOptions{board.previousPly.known};

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
                    x + direction[0],
                    y + direction[1]
                };

                if (!board.inBounds(destination)) {
                    continue;
                }

                TurnPlan candidate;
                candidate.moves.push_back(Move{source, moving, destination});

                if (!validateTurnPlan(
                        board,
                        candidate,
                        ourSpecies_,
                        validationOptions
                    ).valid()) {
                    continue;
                }

                if (firstLegalPlan.moves.empty()) {
                    firstLegalPlan = candidate;
                }

                const int score = destinationPreference(
                    board,
                    source,
                    destination,
                    ourSpecies_,
                    moving
                );

                if (score > bestScore) {
                    bestScore = score;
                    bestPlan = std::move(candidate);
                }
            }
        }
    }

    return bestPlan.moves.empty() ? firstLegalPlan : bestPlan;
}

void Agent::advanceTurn() noexcept {
    if (nextTurnNumber_ < std::numeric_limits<std::uint32_t>::max()) {
        ++nextTurnNumber_;
    }
}

AgentDecision Agent::chooseMove(const BoardState& board) {
    using Clock = std::chrono::steady_clock;
    const auto startedAt = Clock::now();
    const auto searchDeadline = startedAt + options_.searchBudget;

    AgentDecision decision;
    decision.turnNumber = nextTurnNumber_;
    decision.previousPlyWasUnknown = !board.previousPly.known;
    decision.crossPlyRestrictionsChecked = board.previousPly.known;

    const auto finish = [&]() {
        decision.elapsedMicroseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - startedAt
            ).count()
        );
        advanceTurn();
        return decision;
    };

    if (!board.hasValidStorage() ||
        !isPlayableSpecies(ourSpecies_) ||
        board.ourSpecies != ourSpecies_) {
        decision.method = AgentDecisionMethod::InvalidInput;
        return finish();
    }

    // The protocol does not expose the previous opponent MOV. Do not infer
    // exact sources/destinations from sparse UPD cell snapshots; record only
    // what was actually observed.
    const std::uint32_t historyUpdateNumber = nextHistoryUpdateNumber_;
    const HistoryUpdateResult historyUpdate =
        history_.recordObservation(board, historyUpdateNumber);
    decision.historyUpdated = historyUpdate.valid();
    if (decision.historyUpdated &&
        nextHistoryUpdateNumber_ < std::numeric_limits<std::uint32_t>::max()) {
        ++nextHistoryUpdateNumber_;
    }

    MultiMoveCandidateOptions generationOptions =
        options_.candidateGenerationOptions;
    generationOptions.singleMoveOptions.requirePreviousPlyFootprint =
        board.previousPly.known;

    const MultiMoveCandidateResult generated =
        generateMultiMoveCandidates(
            board,
            ourSpecies_,
            generationOptions
        );

    if (generated.valid()) {
        decision.candidatesGenerated = generated.candidates.size();
    }

    // Set both transition validation and opponent candidate generation to
    // match the information actually available in BoardState.
    OpponentSearchOptions searchOptions = options_.opponentSearchOptions;
    searchOptions.rootRankingOptions.transitionOptions.requirePreviousPlyFootprint =
        board.previousPly.known;
    searchOptions.opponentGenerationOptions.singleMoveOptions.requirePreviousPlyFootprint =
        true; // After our simulated move, its footprint is known.

    bool selectedBySearch = false;
    if (generated.valid() && !generated.candidates.empty() &&
        Clock::now() < searchDeadline) {
        OpponentSearchResult search;
        if (decision.historyUpdated) {
            search = searchTurnCandidates(
                board,
                generated.candidates,
                ourSpecies_,
                history_,
                historyUpdateNumber,
                searchOptions
            );
            decision.historyAwareSearchUsed = search.stats.predictionsUsed;
        } else {
            search = searchTurnCandidates(
                board,
                generated.candidates,
                ourSpecies_,
                searchOptions
            );
        }

        decision.rootCandidatesEvaluated = search.stats.rootCandidatesEvaluated;
        decision.opponentResponsesEvaluated = search.stats.opponentResponsesEvaluated;

        if (search.valid() && search.hasBestCandidate &&
            search.bestCandidateIndex < generated.candidates.size()) {
            decision.plan = generated.candidates[search.bestCandidateIndex];
            selectedBySearch = validatePlan(board, decision.plan);
        }
    }

    if (selectedBySearch) {
        decision.method = AgentDecisionMethod::Search;
        return finish();
    }

    // Search may be unavailable early in a game or skipped when candidate
    // generation consumed the configured search budget. Keep a generated,
    // independently validated candidate as the deterministic fallback.
    if (generated.valid()) {
        for (const TurnPlan& candidate : generated.candidates) {
            if (validatePlan(board, candidate)) {
                decision.plan = candidate;
                decision.method = AgentDecisionMethod::CandidateFallback;
                return finish();
            }
        }
    }

    // Last-resort legal one-move policy, independent of the search pipeline.
    decision.plan = makeEmergencyFallback(board);
    decision.method = decision.plan.moves.empty()
        ? AgentDecisionMethod::NoLegalMove
        : AgentDecisionMethod::EmergencyFallback;

    return finish();
}

} // namespace vvw::ngs
