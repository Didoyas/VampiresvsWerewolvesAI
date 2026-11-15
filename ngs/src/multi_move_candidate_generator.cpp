#include "multi_move_candidate_generator.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace vvw::ngs {
namespace {

constexpr std::size_t kNoBucket = static_cast<std::size_t>(-1);

struct SourceBucket {
    Position source{};
    std::vector<Move> moves;
};

MultiMoveCandidateResult failure(MultiMoveCandidateError error) {
    MultiMoveCandidateResult result;
    result.error = error;
    return result;
}

MultiMoveCandidateError mapStageOneError(CandidateGenerationError error) {
    switch (error) {
        case CandidateGenerationError::None:
            return MultiMoveCandidateError::None;
        case CandidateGenerationError::InvalidBoard:
            return MultiMoveCandidateError::InvalidBoard;
        case CandidateGenerationError::InvalidSpecies:
            return MultiMoveCandidateError::InvalidSpecies;
        case CandidateGenerationError::MissingPreviousPlyFootprint:
            return MultiMoveCandidateError::MissingPreviousPlyFootprint;
        case CandidateGenerationError::InvalidSplitRatio:
            return MultiMoveCandidateError::InvalidSplitRatio;
    }
    return MultiMoveCandidateError::InvalidOptions;
}

bool validOptions(const MultiMoveCandidateOptions& options) noexcept {
    return options.maxMovesPerPlan >= 2 &&
           options.maxMovesPerPlan <= 3 &&
           options.variantsPerSourceCombination > 0 &&
           options.maxCombinationAttempts > 0;
}

bool sameMove(const Move& lhs, const Move& rhs) noexcept {
    return lhs.source == rhs.source &&
           lhs.destination == rhs.destination &&
           lhs.count == rhs.count;
}

bool samePlan(const TurnPlan& lhs, const TurnPlan& rhs) noexcept {
    if (lhs.moves.size() != rhs.moves.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.moves.size(); ++i) {
        if (!sameMove(lhs.moves[i], rhs.moves[i])) {
            return false;
        }
    }
    return true;
}

bool alreadyPresent(
    const std::vector<TurnPlan>& plans,
    const TurnPlan& candidate
) noexcept {
    return std::any_of(
        plans.begin(),
        plans.end(),
        [&candidate](const TurnPlan& existing) {
            return samePlan(existing, candidate);
        }
    );
}

// Stage 1 emits candidates grouped by source in row-major order. Build an
// explicit per-cell index so grouping remains linear in the number of moves.
std::vector<SourceBucket> makeSourceBuckets(
    const BoardState& board,
    const CandidateGenerationResult& singles
) {
    std::vector<SourceBucket> buckets;
    std::vector<std::size_t> bucketForCell(board.cells.size(), kNoBucket);

    for (const TurnPlan& plan : singles.candidates) {
        if (plan.moves.size() != 1) {
            continue;
        }

        const Move& move = plan.moves.front();
        const std::size_t cellIndex = board.index(move.source);
        std::size_t bucketIndex = bucketForCell[cellIndex];

        if (bucketIndex == kNoBucket) {
            bucketIndex = buckets.size();
            bucketForCell[cellIndex] = bucketIndex;
            buckets.push_back({move.source, {}});
        }

        buckets[bucketIndex].moves.push_back(move);
    }

    return buckets;
}

// Select a deterministic, well-spread subset of the Stage 1 list. This keeps
// single-move fallbacks while avoiding a row-major prefix bias on large maps.
void appendSampledSingles(
    const CandidateGenerationResult& singles,
    std::size_t limit,
    std::vector<TurnPlan>& output,
    MultiMoveCandidateStats& stats
) {
    const std::size_t available = singles.candidates.size();
    const std::size_t count = std::min(available, limit);

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t index = count == 1
            ? 0
            : (i * (available - 1)) / (count - 1);
        output.push_back(singles.candidates[index]);
        ++stats.singleMoveCandidatesIncluded;
    }
}

std::size_t selectedMoveIndex(
    std::size_t moveCount,
    std::size_t variant,
    std::size_t sourceSlot
) noexcept {
    // Different strides/offsets give each source a different deterministic
    // movement choice while spreading variants over its Stage 1 list.
    const std::size_t stride = 5 + 2 * sourceSlot;
    const std::size_t offset = 3 * sourceSlot;
    return (variant * stride + offset) % moveCount;
}

} // namespace

MultiMoveCandidateResult generateMultiMoveCandidates(
    const BoardState& board,
    Species actingSpecies,
    const MultiMoveCandidateOptions& options
) {
    if (!validOptions(options)) {
        return failure(MultiMoveCandidateError::InvalidOptions);
    }

    const CandidateGenerationResult singles =
        generateSingleMoveCandidates(
            board,
            actingSpecies,
            options.singleMoveOptions
        );

    if (!singles.valid()) {
        return failure(mapStageOneError(singles.error));
    }

    MultiMoveCandidateResult result;
    result.stats.singleMoveCandidatesAvailable = singles.candidates.size();

    const std::vector<SourceBucket> buckets =
        makeSourceBuckets(board, singles);
    result.stats.sourceGroupsAvailable = buckets.size();

    // Keep one-move actions in the portfolio as a safe fallback. Compound
    // quotas are independent so a large single-move list cannot crowd them out.
    appendSampledSingles(
        singles,
        options.maxSingleMoveCandidates,
        result.candidates,
        result.stats
    );

    ValidationOptions validationOptions;
    validationOptions.requirePreviousPlyFootprint =
        options.singleMoveOptions.requirePreviousPlyFootprint;

    const auto appendCombination = [&](const TurnPlan& plan) -> bool {
        if (result.stats.combinationAttempts >=
            options.maxCombinationAttempts) {
            result.stats.combinationBudgetExhausted = true;
            return false;
        }

        ++result.stats.combinationAttempts;

        const ValidationResult validation =
            validateTurnPlan(
                board,
                plan,
                actingSpecies,
                validationOptions
            );

        if (!validation.valid()) {
            ++result.stats.combinationsRejectedByValidator;
            return true;
        }

        if (alreadyPresent(result.candidates, plan)) {
            ++result.stats.duplicatePlansSkipped;
            return true;
        }

        result.candidates.push_back(plan);
        if (plan.moves.size() == 2) {
            ++result.stats.twoMoveCandidatesGenerated;
        } else if (plan.moves.size() == 3) {
            ++result.stats.threeMoveCandidatesGenerated;
        }
        return true;
    };

    // Build pair plans by walking source-group offsets. This spreads the
    // earliest pairs across the board instead of exhausting all variants of
    // the first two source cells before considering other groups.
    const std::size_t pairQuota = options.maxTwoMoveCandidates;
    if (pairQuota > 0 && buckets.size() >= 2) {
        for (std::size_t offset = 1;
             offset < buckets.size() &&
             result.stats.twoMoveCandidatesGenerated < pairQuota;
             ++offset) {
            for (std::size_t first = 0;
                 first + offset < buckets.size() &&
                 result.stats.twoMoveCandidatesGenerated < pairQuota;
                 ++first) {
                const SourceBucket& a = buckets[first];
                const SourceBucket& b = buckets[first + offset];
                const std::size_t variants = std::min(
                    options.variantsPerSourceCombination,
                    std::max(a.moves.size(), b.moves.size())
                );

                for (std::size_t variant = 0;
                     variant < variants &&
                     result.stats.twoMoveCandidatesGenerated < pairQuota;
                     ++variant) {
                    TurnPlan plan;
                    plan.moves.push_back(
                        a.moves[selectedMoveIndex(a.moves.size(), variant, 0)]
                    );
                    plan.moves.push_back(
                        b.moves[selectedMoveIndex(b.moves.size(), variant, 1)]
                    );

                    if (!appendCombination(plan)) {
                        return result;
                    }
                }
            }
        }
    }

    // Three-source plans are generated only when requested. They are more
    // expensive to evaluate later, so their default quota is deliberately
    // smaller than the two-source quota.
    const std::size_t tripleQuota = options.maxThreeMoveCandidates;
    if (options.maxMovesPerPlan >= 3 &&
        tripleQuota > 0 && buckets.size() >= 3) {
        for (std::size_t gap = 1;
             gap + 1 < buckets.size() &&
             result.stats.threeMoveCandidatesGenerated < tripleQuota;
             ++gap) {
            for (std::size_t first = 0;
                 first + gap + 1 < buckets.size() &&
                 result.stats.threeMoveCandidatesGenerated < tripleQuota;
                 ++first) {
                const SourceBucket& a = buckets[first];
                const SourceBucket& b = buckets[first + gap];
                const SourceBucket& c = buckets[first + gap + 1];
                const std::size_t variants = std::min(
                    options.variantsPerSourceCombination,
                    std::max(
                        a.moves.size(),
                        std::max(b.moves.size(), c.moves.size())
                    )
                );

                for (std::size_t variant = 0;
                     variant < variants &&
                     result.stats.threeMoveCandidatesGenerated < tripleQuota;
                     ++variant) {
                    TurnPlan plan;
                    plan.moves.push_back(
                        a.moves[selectedMoveIndex(a.moves.size(), variant, 0)]
                    );
                    plan.moves.push_back(
                        b.moves[selectedMoveIndex(b.moves.size(), variant, 1)]
                    );
                    plan.moves.push_back(
                        c.moves[selectedMoveIndex(c.moves.size(), variant, 2)]
                    );

                    if (!appendCombination(plan)) {
                        return result;
                    }
                }
            }
        }
    }

    return result;
}

} // namespace vvw::ngs
