
#include "opponent_search.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace vvw::ngs {
namespace {

class RunningStatistics {
public:
    void add(double value) noexcept {
        ++count_;

        const double delta = value - mean_;
        mean_ += delta / static_cast<double>(count_);

        const double deltaAfterUpdate = value - mean_;
        squaredDifferences_ += delta * deltaAfterUpdate;
    }

    std::size_t count() const noexcept {
        return count_;
    }

    double mean() const noexcept {
        return mean_;
    }

    double standardDeviation() const noexcept {
        if (count_ < 2) {
            return 0.0;
        }

        const double variance =
            squaredDifferences_ /
            static_cast<double>(count_ - 1);

        return std::sqrt(std::max(0.0, variance));
    }

private:
    std::size_t count_{0};
    double mean_{0.0};
    double squaredDifferences_{0.0};
};

struct ResponseCandidateInfo {
    std::size_t candidateIndex{0};

    double predictionSupport{0.0};
    double tacticalThreat{0.0};

    bool selectedByPrediction{false};
    bool selectedForAdversarialCoverage{false};
};

struct ResponseOutcome {
    double score{0.0};
    double predictionSupport{0.0};
    bool selectedByPrediction{false};
};

bool isPlayableSpecies(Species species) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

Population populationFor(
    const Cell& cell,
    Species species
) noexcept {
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

bool validOptions(
    const OpponentSearchOptions& options
) noexcept {
    if (options.maxRootCandidates == 0 ||
        options.maxOpponentResponses == 0 ||
        options.simulationsPerRootCandidate == 0) {
        return false;
    }

    if (options.maxLikelyOpponentResponses == 0 &&
        options.maxAdversarialOpponentResponses == 0) {
        return false;
    }

    if (!std::isfinite(options.riskAversion) ||
        options.riskAversion < 0.0) {
        return false;
    }

    if (!std::isfinite(options.expectedResponseWeight) ||
        options.expectedResponseWeight < 0.0) {
        return false;
    }

    if (!std::isfinite(options.worstCaseResponseWeight) ||
        options.worstCaseResponseWeight < 0.0) {
        return false;
    }

    return options.expectedResponseWeight +
               options.worstCaseResponseWeight > 0.0;
}

// -------------------------------------------------------------
// Historical prediction matching
// -------------------------------------------------------------

double predictionSupportForPlan(
    const TurnPlan& plan,
    const OpponentPredictionResult* predictions
) noexcept {
    if (predictions == nullptr ||
        predictions->destinations.empty() ||
        plan.moves.empty()) {
        return 0.0;
    }

    double totalSupport = 0.0;

    for (const Move& move : plan.moves) {
        double bestMatchingScore = 0.0;

        for (const PredictedDestination& prediction :
             predictions->destinations) {

            if (prediction.source != move.source ||
                prediction.destination != move.destination) {
                continue;
            }

            if (!std::isfinite(prediction.priorityScore)) {
                continue;
            }

            bestMatchingScore =
                std::max(
                    bestMatchingScore,
                    std::max(
                        0.0,
                        std::min(1.0, prediction.priorityScore)
                    )
                );
        }

        totalSupport += bestMatchingScore;
    }

    // A multi-move response is supported in proportion to
    // the fraction and quality of its historically predicted moves.
    return totalSupport /
           static_cast<double>(plan.moves.size());
}

// -------------------------------------------------------------
// Tactical threat scoring
// -------------------------------------------------------------

double calculateTacticalThreatScore(
    const BoardState& board,
    const TurnPlan& opponentPlan,
    Species opponentSpecies
) noexcept {
    if (opponentPlan.moves.empty()) {
        return 0.0;
    }

    const Species ourSpecies = opponentOf(opponentSpecies);

    double totalThreat = 0.0;

    for (const Move& move : opponentPlan.moves) {
        if (!board.inBounds(move.destination)) {
            continue;
        }

        const Cell& destination =
            board.at(move.destination);

        const Population ourDefenders =
            populationFor(destination, ourSpecies);

        // A possible creature attack is a high-priority threat.
        if (ourDefenders > 0) {
            const std::uint64_t attackers =
                static_cast<std::uint64_t>(move.count);

            const std::uint64_t defenders =
                static_cast<std::uint64_t>(ourDefenders);

            // Use the game's 1.5:1 attack threshold.
            if (2ULL * attackers >= 3ULL * defenders) {
                totalThreat += 3.0;
            } else {
                // A weaker attack can still create pressure or
                // become dangerous when combined with other moves.
                totalThreat += 0.75;
            }
        }

        // Human conversion is strategically useful to the opponent.
        if (destination.humans > 0) {
            totalThreat += 0.75;

            if (move.count >= destination.humans) {
                totalThreat += 1.25;
            }

            if (destination.isHouse) {
                totalThreat += 0.25;
            }
        }
    }

    // Average the threat across moves so that simply having more
    // moves does not automatically make a plan look more dangerous.
    return totalThreat /
           static_cast<double>(opponentPlan.moves.size());
}

// -------------------------------------------------------------
// Select the union of predicted and adversarial responses.
// -------------------------------------------------------------

std::vector<ResponseCandidateInfo> selectOpponentResponses(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species opponentSpecies,
    const OpponentPredictionResult* predictions,
    const OpponentSearchOptions& options
) {
    std::vector<ResponseCandidateInfo> all;
    all.reserve(candidates.size());

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        ResponseCandidateInfo info;

        info.candidateIndex = i;

        info.predictionSupport =
            predictionSupportForPlan(
                candidates[i],
                predictions
            );

        info.tacticalThreat =
            calculateTacticalThreatScore(
                board,
                candidates[i],
                opponentSpecies
            );

        all.push_back(info);
    }

    std::vector<ResponseCandidateInfo> selected;
    selected.reserve(
        std::min(
            options.maxOpponentResponses,
            candidates.size()
        )
    );

    // First select historically supported responses.
    std::vector<std::size_t> predictionOrder(all.size());

    for (std::size_t i = 0; i < all.size(); ++i) {
        predictionOrder[i] = i;
    }

    std::sort(
        predictionOrder.begin(),
        predictionOrder.end(),
        [&all](std::size_t lhs, std::size_t rhs) {
            const ResponseCandidateInfo& a = all[lhs];
            const ResponseCandidateInfo& b = all[rhs];

            if (a.predictionSupport != b.predictionSupport) {
                return a.predictionSupport > b.predictionSupport;
            }

            if (a.tacticalThreat != b.tacticalThreat) {
                return a.tacticalThreat > b.tacticalThreat;
            }

            return a.candidateIndex < b.candidateIndex;
        }
    );

    for (const std::size_t index : predictionOrder) {
        if (selected.size() >= options.maxOpponentResponses ||
            selected.size() >= options.maxLikelyOpponentResponses) {
            break;
        }

        if (all[index].predictionSupport <= 0.0) {
            break;
        }

        ResponseCandidateInfo chosen = all[index];
        chosen.selectedByPrediction = true;

        selected.push_back(chosen);
    }

    // Next add tactically dangerous responses, including responses
    // that are not supported by historical predictions.
    std::vector<std::size_t> tacticalOrder(all.size());

    for (std::size_t i = 0; i < all.size(); ++i) {
        tacticalOrder[i] = i;
    }

    std::sort(
        tacticalOrder.begin(),
        tacticalOrder.end(),
        [&all](std::size_t lhs, std::size_t rhs) {
            const ResponseCandidateInfo& a = all[lhs];
            const ResponseCandidateInfo& b = all[rhs];

            if (a.tacticalThreat != b.tacticalThreat) {
                return a.tacticalThreat > b.tacticalThreat;
            }

            if (a.predictionSupport != b.predictionSupport) {
                return a.predictionSupport > b.predictionSupport;
            }

            return a.candidateIndex < b.candidateIndex;
        }
    );

    std::size_t adversarialSelections = 0;

    for (const std::size_t index : tacticalOrder) {
        const ResponseCandidateInfo& candidate = all[index];

        // Only genuinely threatening responses qualify for the
        // adversarial pool. Since tacticalOrder is descending,
        // no later candidate can have a positive threat either.
        if (candidate.tacticalThreat <= 0.0) {
            break;
        }

        if (adversarialSelections >=
            options.maxAdversarialOpponentResponses) {
            break;
        }

        ResponseCandidateInfo* existing = nullptr;

        for (ResponseCandidateInfo& chosen : selected) {
            if (chosen.candidateIndex == candidate.candidateIndex) {
                existing = &chosen;
                break;
            }
        }

        if (existing != nullptr) {
            // A response can belong to both pools, but only when
            // it has a strictly positive tactical threat score.
            existing->selectedForAdversarialCoverage = true;
            ++adversarialSelections;
            continue;
        }

        if (selected.size() >= options.maxOpponentResponses) {
            // The union budget is full. Continue scanning so an
            // already-selected predicted response can still be
            // identified as adversarial, but do not add new entries.
            continue;
        }

        ResponseCandidateInfo chosen = candidate;
        chosen.selectedForAdversarialCoverage = true;

        selected.push_back(chosen);
        ++adversarialSelections;
    }

    // If neither historical matching nor tactical scoring found a
    // response, retain a small, diverse sample of legal responses.
    // These fallback entries are not falsely labelled predicted or
    // adversarial; they simply prevent a non-terminal search from
    // discarding an otherwise valid opponent turn.
    if (selected.empty() && !candidates.empty()) {
        const std::size_t fallbackCount =
            std::min(options.maxOpponentResponses, candidates.size());

        if (fallbackCount == 1) {
            ResponseCandidateInfo fallback;
            fallback.candidateIndex = candidates.size() / 2;
            selected.push_back(fallback);
        } else {
            for (std::size_t i = 0; i < fallbackCount; ++i) {
                const std::size_t index =
                    i * (candidates.size() - 1) /
                    (fallbackCount - 1);

                ResponseCandidateInfo fallback;
                fallback.candidateIndex = index;
                selected.push_back(fallback);
            }
        }
    }

    return selected;
}

// -------------------------------------------------------------
// Evaluate a board from our perspective.
// -------------------------------------------------------------

bool evaluateForUs(
    const BoardState& board,
    Species ourSpecies,
    Species sideToMove,
    const OpponentSearchOptions& options,
    double& score
) {
    const EvaluationResult evaluation =
        evaluatePosition(
            board,
            ourSpecies,
            sideToMove,
            options.rootRankingOptions.evaluationWeights
        );

    if (!evaluation.valid()) {
        return false;
    }

    score = evaluation.features.totalScore;
    return true;
}

// -------------------------------------------------------------
// Core search implementation.
// -------------------------------------------------------------

OpponentSearchResult searchCore(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const OpponentPredictionResult* predictions,
    const OpponentSearchOptions& options
) {
    OpponentSearchResult result;

    result.stats.rootCandidatesProvided = candidates.size();
    result.stats.predictionsUsed = predictions != nullptr;

    if (!board.hasValidStorage()) {
        result.error = OpponentSearchError::InvalidBoard;
        return result;
    }

    if (!isPlayableSpecies(actingSpecies) ||
        board.ourSpecies != actingSpecies) {
        result.error = OpponentSearchError::InvalidSpecies;
        return result;
    }

    if (candidates.empty()) {
        result.error = OpponentSearchError::EmptyCandidates;
        return result;
    }

    if (!validOptions(options)) {
        result.error = OpponentSearchError::InvalidOptions;
        return result;
    }

    const Species opponentSpecies = opponentOf(actingSpecies);

    // ---------------------------------------------------------
    // Step 1: Rank our root candidates using Stage 3.
    // ---------------------------------------------------------

    const CandidateRankingResult rootRanking =
        rankTurnCandidates(
            board,
            candidates,
            actingSpecies,
            options.rootRankingOptions
        );

    if (!rootRanking.valid()) {
        result.error = OpponentSearchError::RootRankingFailed;
        return result;
    }

    if (rootRanking.ranked.empty()) {
        result.error =
            OpponentSearchError::NoValidRootCandidates;
        return result;
    }

    const std::size_t rootCount =
        std::min(
            options.maxRootCandidates,
            rootRanking.ranked.size()
        );

    result.stats.rootCandidatesShortlisted = rootCount;

    // ---------------------------------------------------------
    // Step 2: Examine each shortlisted root candidate.
    // ---------------------------------------------------------

    for (std::size_t rootRank = 0;
         rootRank < rootCount;
         ++rootRank) {

        const std::size_t originalCandidateIndex =
            rootRanking.ranked[rootRank].candidateIndex;

        if (originalCandidateIndex >= candidates.size()) {
            continue;
        }

        const TurnPlan& ourPlan =
            candidates[originalCandidateIndex];

        RunningStatistics combinedStatistics;
        RunningStatistics expectedStatistics;
        RunningStatistics worstStatistics;

        std::size_t responsesExaminedForCandidate = 0;
        std::size_t predictedResponsesForCandidate = 0;
        std::size_t adversarialResponsesForCandidate = 0;

        // -----------------------------------------------------
        // Step 3: Monte Carlo simulations for stochastic battles.
        // -----------------------------------------------------

        for (std::size_t simulation = 0;
             simulation < options.simulationsPerRootCandidate;
             ++simulation) {

            ++result.stats.rootSimulationsAttempted;

            const std::uint32_t rootSeed =
                options.baseRandomSeed +
                static_cast<std::uint32_t>(simulation) *
                    0x9E3779B9u +
                static_cast<std::uint32_t>(rootRank);

            std::mt19937 ourRandomEngine(rootSeed);

            // Simulate our candidate.
            const TransitionResult ourTransition =
                applyTurnPlan(
                    board,
                    ourPlan,
                    actingSpecies,
                    ourRandomEngine,
                    options.rootRankingOptions.transitionOptions
                );

            if (!ourTransition.valid()) {
                ++result.stats.rootTransitionFailures;
                continue;
            }

            const BoardState& afterOurMove =
                ourTransition.nextState;

            // -------------------------------------------------
            // Step 4: Generate legal opponent responses.
            // -------------------------------------------------

            ++result.stats.opponentPositionsGenerated;

            const MultiMoveCandidateResult opponentGeneration =
                generateMultiMoveCandidates(
                    afterOurMove,
                    opponentSpecies,
                    options.opponentGenerationOptions
                );

            if (!opponentGeneration.valid()) {
                ++result.stats.opponentGenerationFailures;
                continue;
            }

            const std::vector<TurnPlan>& opponentCandidates =
                opponentGeneration.candidates;

            const std::size_t responseCount =
                opponentCandidates.size();

            result.stats.opponentResponsesGenerated += responseCount;

            // -------------------------------------------------
            // Step 5: Handle positions with no legal response.
            // -------------------------------------------------

            if (responseCount == 0) {
                double positionScore = 0.0;

                if (!evaluateForUs(
                        afterOurMove,
                        actingSpecies,
                        opponentSpecies,
                        options,
                        positionScore)) {

                    ++result.stats.evaluationFailures;
                    continue;
                }

                combinedStatistics.add(positionScore);
                expectedStatistics.add(positionScore);
                worstStatistics.add(positionScore);

                ++result.stats.simulationsWithoutOpponentResponse;
                ++result.stats.simulationsCompleted;

                continue;
            }

            // -------------------------------------------------
            // Step 6: Select the union of likely and dangerous
            // responses.
            // -------------------------------------------------

            const std::vector<ResponseCandidateInfo> selected =
                selectOpponentResponses(
                    afterOurMove,
                    opponentCandidates,
                    opponentSpecies,
                    predictions,
                    options
                );

            if (selected.empty()) {
                ++result.stats.opponentResponsesSkippedByBudget;
                continue;
            }

            result.stats.opponentResponsesSkippedByBudget +=
                responseCount - selected.size();

            for (const ResponseCandidateInfo& response : selected) {
                if (response.selectedByPrediction) {
                    ++result.stats.responsesSelectedByPrediction;
                    ++predictedResponsesForCandidate;
                }

                if (response.selectedForAdversarialCoverage) {
                    ++result.stats.responsesSelectedForAdversarialCoverage;
                    ++adversarialResponsesForCandidate;
                }
            }

            // -------------------------------------------------
            // Step 7: Simulate selected opponent responses.
            // -------------------------------------------------

            std::vector<ResponseOutcome> outcomes;
            outcomes.reserve(selected.size());

            for (const ResponseCandidateInfo& response : selected) {
                ++result.stats.opponentResponsesExamined;
                ++responsesExaminedForCandidate;

                if (response.candidateIndex >= responseCount) {
                    continue;
                }

                const TurnPlan& opponentPlan =
                    opponentCandidates[response.candidateIndex];

                // Each candidate gets a reproducible seed derived
                // from the simulation and candidate indices.
                const std::uint32_t responseSeed =
                    options.baseRandomSeed ^
                    (0x85EBCA6Bu +
                     static_cast<std::uint32_t>(simulation) *
                         0x9E3779B9u +
                     static_cast<std::uint32_t>(
                         response.candidateIndex
                     ));

                std::mt19937 opponentRandomEngine(responseSeed);

                const TransitionResult opponentTransition =
                    applyTurnPlan(
                        afterOurMove,
                        opponentPlan,
                        opponentSpecies,
                        opponentRandomEngine,
                        options.rootRankingOptions.transitionOptions
                    );

                if (!opponentTransition.valid()) {
                    ++result.stats.opponentTransitionFailures;
                    continue;
                }

                double responseScore = 0.0;

                // After the opponent's response, it is our turn.
                if (!evaluateForUs(
                        opponentTransition.nextState,
                        actingSpecies,
                        actingSpecies,
                        options,
                        responseScore)) {

                    ++result.stats.evaluationFailures;
                    continue;
                }

                ResponseOutcome outcome;

                outcome.score = responseScore;
                outcome.predictionSupport =
                    response.predictionSupport;
                outcome.selectedByPrediction =
                    response.selectedByPrediction;

                outcomes.push_back(outcome);

                ++result.stats.opponentResponsesEvaluated;
            }

            if (outcomes.empty()) {
                continue;
            }

            // -------------------------------------------------
            // Step 8: Calculate the worst-case response score.
            // -------------------------------------------------

            double worstResponseScore =
                std::numeric_limits<double>::infinity();

            for (const ResponseOutcome& outcome : outcomes) {
                worstResponseScore =
                    std::min(
                        worstResponseScore,
                        outcome.score
                    );
            }

            // -------------------------------------------------
            // Step 9: Calculate prediction-weighted expectation.
            // -------------------------------------------------

            double weightedScoreTotal = 0.0;
            double predictionWeightTotal = 0.0;

            for (const ResponseOutcome& outcome : outcomes) {
                if (!outcome.selectedByPrediction ||
                    outcome.predictionSupport <= 0.0) {
                    continue;
                }

                weightedScoreTotal +=
                    outcome.score * outcome.predictionSupport;

                predictionWeightTotal +=
                    outcome.predictionSupport;
            }

            double expectedResponseScore = 0.0;

            if (predictionWeightTotal > 0.0) {
                expectedResponseScore =
                    weightedScoreTotal / predictionWeightTotal;
            } else {
                // No supported prediction was successfully simulated.
                // Use a uniform average over the valid selected responses.
                for (const ResponseOutcome& outcome : outcomes) {
                    expectedResponseScore += outcome.score;
                }

                expectedResponseScore /=
                    static_cast<double>(outcomes.size());
            }

            // -------------------------------------------------
            // Step 10: Combine expected and worst-case scores.
            // -------------------------------------------------

            const double totalWeight =
                options.expectedResponseWeight +
                options.worstCaseResponseWeight;

            const double combinedScore =
                (
                    options.expectedResponseWeight *
                        expectedResponseScore +
                    options.worstCaseResponseWeight *
                        worstResponseScore
                ) / totalWeight;

            combinedStatistics.add(combinedScore);
            expectedStatistics.add(expectedResponseScore);
            worstStatistics.add(worstResponseScore);

            ++result.stats.simulationsCompleted;
        }

        // -----------------------------------------------------
        // Step 11: Summarise this root candidate.
        // -----------------------------------------------------

        if (combinedStatistics.count() == 0) {
            continue;
        }

        RootCandidateScore score;

        score.candidateIndex = originalCandidateIndex;

        score.meanWorstResponseScore =
            worstStatistics.mean();

        score.meanExpectedResponseScore =
            expectedStatistics.mean();

        score.meanCombinedResponseScore =
            combinedStatistics.mean();

        score.standardDeviation =
            combinedStatistics.standardDeviation();

        score.rankingScore =
            score.meanCombinedResponseScore -
            options.riskAversion * score.standardDeviation;

        score.simulationsCompleted =
            combinedStatistics.count();

        score.opponentResponsesExamined =
            responsesExaminedForCandidate;

        score.predictedResponsesExamined =
            predictedResponsesForCandidate;

        score.adversarialResponsesExamined =
            adversarialResponsesForCandidate;

        result.ranked.push_back(score);

        ++result.stats.rootCandidatesEvaluated;
    }

    // ---------------------------------------------------------
    // Step 12: Sort our root candidates.
    // ---------------------------------------------------------

    if (result.ranked.empty()) {
        result.error =
            OpponentSearchError::NoValidSearchResults;
        return result;
    }

    std::sort(
        result.ranked.begin(),
        result.ranked.end(),
        [](const RootCandidateScore& lhs,
           const RootCandidateScore& rhs) {

            if (lhs.rankingScore != rhs.rankingScore) {
                return lhs.rankingScore > rhs.rankingScore;
            }

            return lhs.candidateIndex < rhs.candidateIndex;
        }
    );

    result.hasBestCandidate = true;
    result.bestCandidateIndex =
        result.ranked.front().candidateIndex;

    result.error = OpponentSearchError::None;

    return result;
}

} // namespace

// -------------------------------------------------------------
// Original history-independent API.
// -------------------------------------------------------------

OpponentSearchResult searchTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const OpponentSearchOptions& options
) {
    return searchCore(
        board,
        candidates,
        actingSpecies,
        nullptr,
        options
    );
}

// -------------------------------------------------------------
// New history-aware API.
// -------------------------------------------------------------

OpponentSearchResult searchTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const OpponentHistory& history,
    std::uint32_t currentTurn,
    const OpponentSearchOptions& options
) {
    const OpponentPredictionResult predictions =
        predictOpponentBehaviour(
            board,
            history,
            currentTurn
        );

    if (predictions.valid()) {
        return searchCore(
            board,
            candidates,
            actingSpecies,
            &predictions,
            options
        );
    }

    // Predictions can be unavailable early in a game or when
    // history is incomplete. Do not disable search entirely.
    // Fall back to tactical response selection.
    OpponentSearchResult result =
        searchCore(
            board,
            candidates,
            actingSpecies,
            nullptr,
            options
        );

    result.predictionError = predictions.error;
    result.stats.predictionFallbackUsed = true;

    return result;
}

} // namespace vvw::ngs
