
#include "opponent_search.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace vvw::ngs {
namespace {

// Welford's online algorithm for mean and variance.
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
            squaredDifferences_ / static_cast<double>(count_ - 1);

        return std::sqrt(std::max(0.0, variance));
    }

private:
    std::size_t count_{0};
    double mean_{0.0};
    double squaredDifferences_{0.0};
};

// Selects evenly spaced indices from an ordered candidate vector.
//
// This keeps the response budget bounded while sampling across the
// generated candidate list rather than always taking its first entries.
std::vector<std::size_t> selectResponseIndices(
    std::size_t candidateCount,
    std::size_t maximumResponses
) {
    std::vector<std::size_t> selected;

    if (candidateCount == 0 || maximumResponses == 0) {
        return selected;
    }

    const std::size_t numberToSelect =
        std::min(candidateCount, maximumResponses);

    selected.reserve(numberToSelect);

    if (numberToSelect == candidateCount) {
        for (std::size_t i = 0; i < candidateCount; ++i) {
            selected.push_back(i);
        }
        return selected;
    }

    if (numberToSelect == 1) {
        selected.push_back(candidateCount / 2);
        return selected;
    }

    for (std::size_t i = 0; i < numberToSelect; ++i) {
        const std::size_t index =
            i * (candidateCount - 1) / (numberToSelect - 1);

        selected.push_back(index);
    }

    return selected;
}

bool validOptions(const OpponentSearchOptions& options) noexcept {
    return options.maxRootCandidates > 0 &&
           options.maxOpponentResponses > 0 &&
           options.simulationsPerRootCandidate > 0 &&
           std::isfinite(options.riskAversion) &&
           options.riskAversion >= 0.0;
}

} // namespace

OpponentSearchResult searchTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const OpponentSearchOptions& options
) {
    OpponentSearchResult result;

    result.stats.rootCandidatesProvided = candidates.size();

    if (!board.hasValidStorage()) {
        result.error = OpponentSearchError::InvalidBoard;
        return result;
    }

    if (actingSpecies != Species::Vampires &&
        actingSpecies != Species::Werewolves) {
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

    // ------------------------------------------------------------
    // Step 1: Rank our candidate plans using Stage 3.
    // ------------------------------------------------------------

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
        result.error = OpponentSearchError::NoValidRootCandidates;
        return result;
    }

    // Stage 3 returns candidates sorted from best to worst.
    const std::size_t rootCount =
        std::min(
            options.maxRootCandidates,
            rootRanking.ranked.size()
        );

    result.stats.rootCandidatesShortlisted = rootCount;

    // ------------------------------------------------------------
    // Step 2: Search each shortlisted root candidate.
    // ------------------------------------------------------------

    for (std::size_t rootRank = 0; rootRank < rootCount; ++rootRank) {
        const std::size_t originalCandidateIndex =
            rootRanking.ranked[rootRank].candidateIndex;

        const TurnPlan& ourPlan = candidates[originalCandidateIndex];

        RunningStatistics candidateStatistics;

        std::size_t responsesExaminedForCandidate = 0;

        // Each simulation starts from the original board.
        for (std::size_t simulation = 0;
             simulation < options.simulationsPerRootCandidate;
             ++simulation) {

            ++result.stats.rootSimulationsAttempted;

            const std::uint32_t simulationSeed =
                options.baseRandomSeed +
                static_cast<std::uint32_t>(simulation);

            // ----------------------------------------------------
            // Step 2a: Simulate our candidate move.
            // ----------------------------------------------------

            std::mt19937 ourRandomEngine(simulationSeed);

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

            // ----------------------------------------------------
            // Step 2b: Generate legal opponent responses.
            // ----------------------------------------------------

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

            const std::size_t responseCount =
                opponentGeneration.candidates.size();

            result.stats.opponentResponsesGenerated += responseCount;

            // If no opponent response is available, evaluate the
            // position after our move.
            if (responseCount == 0) {
                const EvaluationResult evaluation =
                    evaluatePosition(
                        afterOurMove,
                        actingSpecies,
                        opponentSpecies,
                        options.rootRankingOptions.evaluationWeights
                    );

                if (!evaluation.valid()) {
                    ++result.stats.evaluationFailures;
                    continue;
                }

                candidateStatistics.add(
                    evaluation.features.totalScore
                );

                ++result.stats.simulationsWithoutOpponentResponse;
                ++result.stats.simulationsCompleted;
                continue;
            }

            // ----------------------------------------------------
            // Step 2c: Bound the number of opponent responses.
            // ----------------------------------------------------

            const std::vector<std::size_t> responseIndices =
                selectResponseIndices(
                    responseCount,
                    options.maxOpponentResponses
                );

            result.stats.opponentResponsesSkippedByBudget +=
                responseCount - responseIndices.size();

            double worstResponseScore =
                std::numeric_limits<double>::infinity();

            bool foundValidResponse = false;

            // ----------------------------------------------------
            // Step 2d: Simulate each selected opponent response.
            // ----------------------------------------------------

            for (const std::size_t responseIndex : responseIndices) {
                ++result.stats.opponentResponsesExamined;
                ++responsesExaminedForCandidate;

                const TurnPlan& opponentPlan =
                    opponentGeneration.candidates[responseIndex];

                // Use the same seed for every response at this
                // simulated position. This provides a common
                // random-number baseline for comparisons.
                std::mt19937 opponentRandomEngine(
                    options.baseRandomSeed +
                    0x9E3779B9u +
                    static_cast<std::uint32_t>(simulation)
                );

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

                // The opponent has moved. It is now our turn again.
                const EvaluationResult evaluation =
                    evaluatePosition(
                        opponentTransition.nextState,
                        actingSpecies,
                        actingSpecies,
                        options.rootRankingOptions.evaluationWeights
                    );

                if (!evaluation.valid()) {
                    ++result.stats.evaluationFailures;
                    continue;
                }

                const double responseScore =
                    evaluation.features.totalScore;

                // The opponent chooses the response least favourable
                // to our species.
                worstResponseScore =
                    std::min(worstResponseScore, responseScore);

                foundValidResponse = true;
            }

            if (!foundValidResponse) {
                // Do not silently treat failed opponent transitions
                // as harmless responses.
                continue;
            }

            candidateStatistics.add(worstResponseScore);
            ++result.stats.simulationsCompleted;
        }

        // --------------------------------------------------------
        // Step 3: Calculate the root candidate's final score.
        // --------------------------------------------------------

        if (candidateStatistics.count() == 0) {
            continue;
        }

        RootCandidateScore score;

        score.candidateIndex = originalCandidateIndex;
        score.meanWorstResponseScore =
            candidateStatistics.mean();
        score.standardDeviation =
            candidateStatistics.standardDeviation();
        score.rankingScore =
            score.meanWorstResponseScore -
            options.riskAversion * score.standardDeviation;
        score.simulationsCompleted =
            candidateStatistics.count();
        score.opponentResponsesExamined =
            responsesExaminedForCandidate;

        result.ranked.push_back(score);
        ++result.stats.rootCandidatesEvaluated;
    }

    // ------------------------------------------------------------
    // Step 4: Sort our candidates by their opponent-aware scores.
    // ------------------------------------------------------------

    if (result.ranked.empty()) {
        result.error = OpponentSearchError::NoValidSearchResults;
        return result;
    }

    std::sort(
        result.ranked.begin(),
        result.ranked.end(),
        [](const RootCandidateScore& lhs,
           const RootCandidateScore& rhs) {
            return lhs.rankingScore > rhs.rankingScore;
        }
    );

    result.hasBestCandidate = true;
    result.bestCandidateIndex =
        result.ranked.front().candidateIndex;

    result.error = OpponentSearchError::None;
    return result;
}

} // namespace vvw::ngs
