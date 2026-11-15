#include "candidate_ranker.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace vvw::ngs {
namespace {

// -----------------------------------------------------------------------------
// Species validation
// -----------------------------------------------------------------------------

bool isPlayableSpecies(Species species) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

// -----------------------------------------------------------------------------
// Running statistics
// -----------------------------------------------------------------------------

// Welford's online algorithm calculates the mean and variance
// without storing every simulation score.
struct ScoreAccumulator {
    std::size_t count{0};
    double mean{0.0};
    double sumSquaredDifferences{0.0};

    void add(double value) noexcept {
        ++count;

        const double difference = value - mean;
        mean += difference / static_cast<double>(count);

        const double differenceAfterUpdate = value - mean;

        sumSquaredDifferences +=
            difference * differenceAfterUpdate;
    }

    double standardDeviation() const noexcept {
        if (count <= 1) {
            return 0.0;
        }

        // Population standard deviation across completed simulations.
        const double variance =
            sumSquaredDifferences / static_cast<double>(count);

        return std::sqrt(std::max(0.0, variance));
    }
};

// -----------------------------------------------------------------------------
// Candidate budget
// -----------------------------------------------------------------------------

// Select candidates evenly across the input vector.
// This preserves representation from the different candidate classes
// when the total exceeds maxCandidatesToEvaluate.
std::vector<std::size_t> selectCandidateIndices(
    std::size_t totalCandidates,
    std::size_t maximumCandidates
) {
    std::vector<std::size_t> indices;

    if (totalCandidates == 0 || maximumCandidates == 0) {
        return indices;
    }

    const std::size_t selectedCount =
        std::min(totalCandidates, maximumCandidates);

    indices.reserve(selectedCount);

    if (selectedCount == totalCandidates) {
        for (std::size_t i = 0; i < totalCandidates; ++i) {
            indices.push_back(i);
        }

        return indices;
    }

    if (selectedCount == 1) {
        indices.push_back(0);
        return indices;
    }

    for (std::size_t i = 0; i < selectedCount; ++i) {
        const std::size_t index =
            i * (totalCandidates - 1) / (selectedCount - 1);

        indices.push_back(index);
    }

    return indices;
}

// -----------------------------------------------------------------------------
// Failure construction
// -----------------------------------------------------------------------------

CandidateRankingResult failure(
    CandidateRankingError error
) {
    CandidateRankingResult result;
    result.error = error;
    return result;
}

} // namespace


// -----------------------------------------------------------------------------
// Public candidate ranking function
// -----------------------------------------------------------------------------

CandidateRankingResult rankTurnCandidates(
    const BoardState& board,
    const std::vector<TurnPlan>& candidates,
    Species actingSpecies,
    const CandidateRankingOptions& options
) {
    // 1. Validate input board.
    if (!board.hasValidStorage()) {
        return failure(CandidateRankingError::InvalidBoard);
    }

    // 2. Validate species.
    if (!isPlayableSpecies(actingSpecies)) {
        return failure(CandidateRankingError::InvalidSpecies);
    }

    // 3. Require at least one candidate.
    if (candidates.empty()) {
        return failure(CandidateRankingError::EmptyCandidates);
    }

    // 4. Validate ranking options.
    if (options.simulationsPerCandidate == 0 ||
        options.maxCandidatesToEvaluate == 0 ||
        !std::isfinite(options.riskAversion) ||
        options.riskAversion < 0.0) {
        return failure(CandidateRankingError::InvalidOptions);
    }

    CandidateRankingResult result;

    result.stats.candidatesProvided = candidates.size();

    // 5. Select candidates within the evaluation budget.
    const std::vector<std::size_t> selectedIndices =
        selectCandidateIndices(
            candidates.size(),
            options.maxCandidatesToEvaluate
        );

    result.stats.candidatesSelectedForEvaluation =
        selectedIndices.size();

    result.stats.candidatesSkippedByBudget =
        candidates.size() - selectedIndices.size();

    const Species nextPlayer = opponentOf(actingSpecies);

    result.ranked.reserve(selectedIndices.size());

    // 6. Evaluate each selected candidate.
    for (const std::size_t candidateIndex : selectedIndices) {
        const TurnPlan& candidate = candidates[candidateIndex];

        ScoreAccumulator scores;

        bool stochasticCandidate = false;
        bool firstSimulationCompleted = false;

        // Each rollout starts from the same original board.
        for (std::size_t simulation = 0;
             simulation < options.simulationsPerCandidate;
             ++simulation) {

            ++result.stats.simulationsAttempted;

            // Use the same sequence of seeds for every candidate.
            // This makes repeated comparisons reproducible.
            const std::uint32_t seed =
                options.baseRandomSeed +
                static_cast<std::uint32_t>(simulation);

            std::mt19937 randomEngine(seed);

            // -------------------------------------------------------------
            // Step A: simulate our complete movement plan.
            // -------------------------------------------------------------

            const TransitionResult transition =
                applyTurnPlan(
                    board,
                    candidate,
                    actingSpecies,
                    randomEngine,
                    options.transitionOptions
                );

            if (!transition.valid()) {
                ++result.stats.transitionFailures;
                ++result.stats.simulationsFailed;
                continue;
            }

            // -------------------------------------------------------------
            // Step B: evaluate the resulting board.
            // -------------------------------------------------------------

            const EvaluationResult evaluation =
                evaluatePosition(
                    transition.nextState,
                    actingSpecies,
                    nextPlayer,
                    options.evaluationWeights
                );

            if (!evaluation.valid()) {
                ++result.stats.evaluationFailures;
                ++result.stats.simulationsFailed;
                continue;
            }

            scores.add(evaluation.features.totalScore);

            ++result.stats.simulationsSucceeded;

            // The transition engine reports how many random battles
            // occurred during this simulation.
            if (simulation == 0) {
                stochasticCandidate =
                    transition.stats.randomBattles > 0;
            }

            firstSimulationCompleted = true;

            // If the first successful simulation contains no random
            // battles, the same plan produces the same result every time.
            // There is no benefit in repeating it.
            if (simulation == 0 && !stochasticCandidate) {
                break;
            }
        }

        // 7. Skip candidates for which no simulation succeeded.
        if (scores.count == 0) {
            continue;
        }

        ++result.stats.candidatesEvaluated;

        if (stochasticCandidate) {
            ++result.stats.stochasticCandidates;
        } else {
            ++result.stats.deterministicCandidates;
        }

        // 8. Calculate candidate score.
        CandidateScore candidateScore;

        candidateScore.candidateIndex = candidateIndex;
        candidateScore.meanScore = scores.mean;
        candidateScore.standardDeviation =
            scores.standardDeviation();

        candidateScore.rankingScore =
            candidateScore.meanScore -
            options.riskAversion *
                candidateScore.standardDeviation;

        candidateScore.simulationsCompleted = scores.count;

        result.ranked.push_back(candidateScore);

        (void)firstSimulationCompleted;
    }

    // 9. If every candidate failed, return an error.
    if (result.ranked.empty()) {
        result.error = CandidateRankingError::NoValidCandidates;
        return result;
    }

    // 10. Sort from best to worst.
    std::sort(
        result.ranked.begin(),
        result.ranked.end(),
        [](const CandidateScore& lhs, const CandidateScore& rhs) {
            if (lhs.rankingScore != rhs.rankingScore) {
                return lhs.rankingScore > rhs.rankingScore;
            }

            if (lhs.meanScore != rhs.meanScore) {
                return lhs.meanScore > rhs.meanScore;
            }

            // Stable, deterministic tie-break.
            return lhs.candidateIndex < rhs.candidateIndex;
        }
    );

    return result;
}

} // namespace vvw::ngs