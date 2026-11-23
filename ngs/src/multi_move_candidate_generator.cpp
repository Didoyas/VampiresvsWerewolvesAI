
#include "multi_move_candidate_generator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace vvw::ngs {
namespace {

constexpr std::size_t kNoBucket = static_cast<std::size_t>(-1);

struct ScoredMove {
    Move move{};
    double priority{0.0};
};

struct SourceBucket {
    Position source{};
    Population sourcePopulation{0};
    double priority{0.0};
    std::vector<ScoredMove> moves;
};

struct DestinationBucket {
    Position destination{};
    double priority{0.0};
    std::vector<ScoredMove> incoming;
};

enum class CompoundPlanKind {
    General,
    Split,
    Merge
};

struct HouseObjective {
    Position position{};
    Population humans{0};
};

Population populationFor(const Cell& cell, Species species) noexcept {
    if (species == Species::Vampires) {
        return cell.vampires;
    }
    if (species == Species::Werewolves) {
        return cell.werewolves;
    }
    return 0;
}

int chebyshevDistance(Position lhs, Position rhs) noexcept {
    const int dx = lhs.x > rhs.x ? lhs.x - rhs.x : rhs.x - lhs.x;
    const int dy = lhs.y > rhs.y ? lhs.y - rhs.y : rhs.y - lhs.y;
    return dx > dy ? dx : dy;
}

// A cheap pre-ranking score used only to decide which legal moves survive the
// bounded candidate portfolio. The full transition/evaluation/search remains
// authoritative. House progress is rewarded only when the moving group (plus
// friendly creatures already at the destination) is strong enough to capture
// that house, preventing weak fragments from dominating the shortlist.
double scoreSingleMovePriority(
    const BoardState& board,
    Species actingSpecies,
    const Move& move,
    const std::vector<HouseObjective>& houseObjectives,
    SearchDeadline deadline,
    bool& timedOut
) noexcept {
    const Cell& sourceCell = board.at(move.source);
    const Cell& destinationCell = board.at(move.destination);
    const Population sourcePopulation = populationFor(sourceCell, actingSpecies);
    const Population friendlyAtDestination =
        populationFor(destinationCell, actingSpecies);
    const Population enemyAtDestination =
        populationFor(destinationCell, opponentOf(actingSpecies));

    const std::uint64_t effectiveDestinationPopulation =
        static_cast<std::uint64_t>(move.count) +
        static_cast<std::uint64_t>(friendlyAtDestination);

    double priority = 0.0;

    // Immediate human conversion is more valuable than merely approaching a
    // target. Houses receive an additional strategic bonus.
    if (destinationCell.humans > 0 &&
        effectiveDestinationPopulation >= destinationCell.humans) {
        priority += 1000.0 +
            10.0 * static_cast<double>(destinationCell.humans) +
            (destinationCell.isHouse ? 100.0 : 0.0);
    }

    // Prefer a legal move that wins an immediate fight. The full transition
    // simulation still determines the actual stochastic outcome.
    if (enemyAtDestination > 0) {
        if (2ULL * effectiveDestinationPopulation >=
            3ULL * static_cast<std::uint64_t>(enemyAtDestination)) {
            priority += 500.0 + static_cast<double>(enemyAtDestination);
        } else {
            // Keep probabilistic attacks in the portfolio. The full transition
            // and risk-aware ranker should decide whether the odds are worth it.
            priority += 1.0;
        }
    }

    // A move into a friendly-occupied cell can consolidate two groups.
    if (friendlyAtDestination > 0) {
        priority += 5.0 + 0.05 * static_cast<double>(
            std::min(move.count, friendlyAtDestination));
    }

    // Score progress towards viable houses. We deliberately keep this cheap:
    // the competition maps contain relatively few houses compared with cells,
    // so this loop scales with active objectives rather than all board cells.
    double bestHouseProgress = 0.0;
    for (const HouseObjective& objective : houseObjectives) {
        if (deadlineReached(deadline)) {
            timedOut = true;
            break;
        }
        if (effectiveDestinationPopulation < objective.humans) {
            continue;
        }

        const int sourceDistance =
            chebyshevDistance(move.source, objective.position);
        const int destinationDistance =
            chebyshevDistance(move.destination, objective.position);
        if (destinationDistance >= sourceDistance) {
            continue;
        }

        const double before = 1.0 / (1.0 + sourceDistance);
        const double after = 1.0 / (1.0 + destinationDistance);
        const double houseWeight =
            4.0 * static_cast<double>(objective.humans);
        const double progress = houseWeight * (after - before);
        if (progress > bestHouseProgress) {
            bestHouseProgress = progress;
        }
    }
    priority += 30.0 * bestHouseProgress;

    // Avoid choosing a split that strands a tiny remnant at the source. This
    // is a modest tie-break penalty, not a legality rule or hard constraint.
    const Population remainingAtSource = sourcePopulation - move.count;
    if (remainingAtSource > 0 && remainingAtSource < 3) {
        priority -= 2.0;
    }

    // Deterministic fallback preference when there is no immediate tactical
    // or house-progress signal: preserve the value of larger useful groups.
    priority += 0.001 * static_cast<double>(
        std::min<Population>(move.count, 100));

    return priority;
}

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
           options.maxMovesPerPlan <= 4 &&
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
    Species actingSpecies,
    const CandidateGenerationResult& singles,
    const std::vector<HouseObjective>& houseObjectives,
    SearchDeadline deadline,
    bool& timedOut
) {
    std::vector<SourceBucket> buckets;
    std::vector<std::size_t> bucketForCell(board.cells.size(), kNoBucket);

    for (const TurnPlan& plan : singles.candidates) {
        if (deadlineReached(deadline)) {
            timedOut = true;
            break;
        }

        if (plan.moves.size() != 1) {
            continue;
        }

        const Move& move = plan.moves.front();
        const std::size_t cellIndex = board.index(move.source);
        std::size_t bucketIndex = bucketForCell[cellIndex];

        if (bucketIndex == kNoBucket) {
            bucketIndex = buckets.size();
            bucketForCell[cellIndex] = bucketIndex;
            buckets.push_back({
                move.source,
                populationFor(board.at(move.source), actingSpecies),
                0.0,
                {}
            });
        }

        const double priority = scoreSingleMovePriority(
            board, actingSpecies, move, houseObjectives, deadline, timedOut
        );
        SourceBucket& bucket = buckets[bucketIndex];
        bucket.moves.push_back({move, priority});
        if (priority > bucket.priority || bucket.moves.size() == 1) {
            bucket.priority = priority;
        }
        if (timedOut) {
            break;
        }
    }

    // Rank moves within each source group, then rank source groups by their
    // best available action. This makes the portfolio independent of which
    // source happened to appear early in the row-major board scan.
    for (SourceBucket& bucket : buckets) {
        std::stable_sort(
            bucket.moves.begin(),
            bucket.moves.end(),
            [](const ScoredMove& lhs, const ScoredMove& rhs) {
                if (lhs.priority != rhs.priority) {
                    return lhs.priority > rhs.priority;
                }
                if (lhs.move.count != rhs.move.count) {
                    return lhs.move.count > rhs.move.count;
                }
                if (lhs.move.destination.y != rhs.move.destination.y) {
                    return lhs.move.destination.y < rhs.move.destination.y;
                }
                return lhs.move.destination.x < rhs.move.destination.x;
            }
        );
    }

    std::stable_sort(
        buckets.begin(),
        buckets.end(),
        [](const SourceBucket& lhs, const SourceBucket& rhs) {
            if (lhs.priority != rhs.priority) {
                return lhs.priority > rhs.priority;
            }
            if (lhs.sourcePopulation != rhs.sourcePopulation) {
                return lhs.sourcePopulation > rhs.sourcePopulation;
            }
            if (lhs.source.y != rhs.source.y) {
                return lhs.source.y < rhs.source.y;
            }
            return lhs.source.x < rhs.source.x;
        }
    );

    return buckets;
}

// Index candidate arrivals by destination. A source contributes at most
// two ranked movement sizes to a given destination, keeping merge sampling
// bounded while retaining a useful amount of population flexibility.
std::vector<DestinationBucket> makeDestinationBuckets(
    const BoardState& board,
    Species actingSpecies,
    const std::vector<SourceBucket>& sources,
    SearchDeadline deadline,
    bool& timedOut
) {
    std::vector<DestinationBucket> buckets;
    std::vector<std::size_t> bucketForCell(board.cells.size(), kNoBucket);

    std::vector<std::uint8_t> variantsForDestination(board.cells.size(), 0);
    std::vector<std::size_t> touchedDestinations;

    for (const SourceBucket& source : sources) {
        touchedDestinations.clear();
        for (const ScoredMove& scored : source.moves) {
            if (deadlineReached(deadline)) {
                timedOut = true;
                return buckets;
            }

            const std::size_t destinationIndex =
                board.index(scored.move.destination);
            if (variantsForDestination[destinationIndex] == 0) {
                touchedDestinations.push_back(destinationIndex);
            }
            if (variantsForDestination[destinationIndex] >= 2) {
                continue;
            }
            ++variantsForDestination[destinationIndex];

            std::size_t bucketIndex = bucketForCell[destinationIndex];
            if (bucketIndex == kNoBucket) {
                bucketIndex = buckets.size();
                bucketForCell[destinationIndex] = bucketIndex;
                buckets.push_back({scored.move.destination, scored.priority, {}});
            }

            DestinationBucket& bucket = buckets[bucketIndex];
            bucket.incoming.push_back(scored);
            if (scored.priority > bucket.priority) {
                bucket.priority = scored.priority;
            }
        }
        for (const std::size_t destinationIndex : touchedDestinations) {
            variantsForDestination[destinationIndex] = 0;
        }
    }

    // A merge can be strategically valuable even when neither incoming
    // group can capture the target alone. Raise destinations where the best
    // pair crosses a deterministic conversion or combat threshold.
    for (DestinationBucket& bucket : buckets) {
        const Cell& target = board.at(bucket.destination);
        const Population friendlyAtTarget = populationFor(target, actingSpecies);
        const Population enemyAtTarget =
            populationFor(target, opponentOf(actingSpecies));

        double combinedPriority = bucket.priority;
        for (std::size_t i = 0; i < bucket.incoming.size(); ++i) {
            for (std::size_t j = i + 1; j < bucket.incoming.size(); ++j) {
                if (deadlineReached(deadline)) {
                    timedOut = true;
                    return buckets;
                }
                const Move& first = bucket.incoming[i].move;
                const Move& second = bucket.incoming[j].move;
                if (first.source == second.source) {
                    continue;
                }

                const std::uint64_t arriving =
                    static_cast<std::uint64_t>(first.count) +
                    static_cast<std::uint64_t>(second.count);
                const std::uint64_t totalFriendly =
                    arriving + static_cast<std::uint64_t>(friendlyAtTarget);

                if (target.humans > 0 && totalFriendly >= target.humans) {
                    const double conversionPriority =
                        1200.0 + 10.0 * static_cast<double>(target.humans) +
                        (target.isHouse ? 150.0 : 0.0);
                    combinedPriority = std::max(
                        combinedPriority, conversionPriority
                    );
                }

                if (enemyAtTarget > 0 &&
                    2ULL * totalFriendly >=
                        3ULL * static_cast<std::uint64_t>(enemyAtTarget)) {
                    combinedPriority = std::max(
                        combinedPriority,
                        700.0 + static_cast<double>(enemyAtTarget)
                    );
                }

                if (friendlyAtTarget > 0) {
                    combinedPriority = std::max(combinedPriority, 8.0);
                }
            }
        }
        bucket.priority = combinedPriority;
    }

    std::stable_sort(
        buckets.begin(), buckets.end(),
        [](const DestinationBucket& lhs, const DestinationBucket& rhs) {
            if (lhs.priority != rhs.priority) {
                return lhs.priority > rhs.priority;
            }
            if (lhs.destination.y != rhs.destination.y) {
                return lhs.destination.y < rhs.destination.y;
            }
            return lhs.destination.x < rhs.destination.x;
        }
    );

    return buckets;
}

// Select singles round-robin by source group. The first pass gives each
// strategically ranked group one opportunity; later passes add alternatives
// from groups that have multiple promising directions/split sizes.
void appendSelectedSingles(
    const std::vector<SourceBucket>& buckets,
    std::size_t limit,
    std::vector<TurnPlan>& output,
    MultiMoveCandidateStats& stats
) {
    for (std::size_t moveRank = 0;
         output.size() < limit;
         ++moveRank) {
        bool includedAny = false;

        for (const SourceBucket& bucket : buckets) {
            if (output.size() >= limit) {
                break;
            }
            if (moveRank >= bucket.moves.size()) {
                continue;
            }

            TurnPlan plan;
            plan.moves.push_back(bucket.moves[moveRank].move);
            output.push_back(std::move(plan));
            ++stats.singleMoveCandidatesIncluded;
            if (moveRank == 0) {
                ++stats.singleMoveSourceGroupsCovered;
            }
            includedAny = true;
        }

        if (!includedAny) {
            break;
        }
    }
}

std::size_t selectedMoveIndex(
    std::size_t moveCount,
    std::size_t variant
) noexcept {
    // Moves are sorted by cheap strategic priority, so variant 0 is the best
    // action for the source and later variants preserve useful alternatives.
    return std::min(variant, moveCount - 1);
}

} // namespace

MultiMoveCandidateResult generateMultiMoveCandidates(
    const BoardState& board,
    Species actingSpecies,
    const MultiMoveCandidateOptions& options,
    SearchDeadline deadline
) {
    if (!validOptions(options)) {
        return failure(MultiMoveCandidateError::InvalidOptions);
    }

    const CandidateGenerationResult singles =
        generateSingleMoveCandidates(
            board,
            actingSpecies,
            options.singleMoveOptions,
            deadline
        );

    if (!singles.valid()) {
        return failure(mapStageOneError(singles.error));
    }

    MultiMoveCandidateResult result;
    result.stats.singleMoveCandidatesAvailable = singles.candidates.size();

    // Build and rank source buckets before applying the bounded single-move
    // quota. This replaces row-major index sampling with strategic selection.
    bool helperTimedOut = false;
    std::vector<HouseObjective> houseObjectives;
    for (int y = 0; y < board.rows; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            const Cell& cell = board.at({x, y});
            if (cell.isHouse && cell.humans > 0) {
                houseObjectives.push_back({{x, y}, cell.humans});
            }
        }
    }
    const std::vector<SourceBucket> buckets = makeSourceBuckets(
        board, actingSpecies, singles, houseObjectives,
        deadline, helperTimedOut
    );
    result.stats.sourceGroupsAvailable = buckets.size();

    // Preserve a safe, diverse one-move portfolio before spending time on
    // compound candidates. These plans were already validated in Stage 1.
    appendSelectedSingles(
        buckets,
        options.maxSingleMoveCandidates,
        result.candidates,
        result.stats
    );

    if (singles.timedOut || helperTimedOut ||
        deadlineReached(deadline)) {
        result.timedOut = true;
        return result;
    }

    ValidationOptions validationOptions;
    validationOptions.requirePreviousPlyFootprint =
        options.singleMoveOptions.requirePreviousPlyFootprint;

    const auto appendCombination = [&, validationOptions](
        const TurnPlan& plan,
        CompoundPlanKind kind = CompoundPlanKind::General
    ) -> bool {
        if (deadlineReached(deadline)) {
            result.timedOut = true;
            return false;
        }

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
            if (kind == CompoundPlanKind::Split) {
                ++result.stats.splitPlansGenerated;
            } else if (kind == CompoundPlanKind::Merge) {
                ++result.stats.mergePlansGenerated;
            }
        } else if (plan.moves.size() == 3) {
            ++result.stats.threeMoveCandidatesGenerated;
        } else if (plan.moves.size() == 4) {
            ++result.stats.fourMoveCandidatesGenerated;
        }

        return true;
    };

    const std::size_t pairQuota = options.maxTwoMoveCandidates;

    // First generate explicit split plans: one source distributes part of
    // its population to two different neighbouring destinations. The total
    // outgoing population is checked here cheaply and then revalidated by
    // validateTurnPlan() before the plan enters the search portfolio.
    const std::size_t splitQuota = options.maxSplitCandidates;
    if (splitQuota > 0 && pairQuota > 0) {
        for (const SourceBucket& source : buckets) {
            if (result.stats.splitPlansGenerated >= splitQuota ||
                result.stats.twoMoveCandidatesGenerated >= pairQuota ||
                result.timedOut) {
                break;
            }

            for (std::size_t i = 0;
                 i < source.moves.size() &&
                 result.stats.splitPlansGenerated < splitQuota &&
                 result.stats.twoMoveCandidatesGenerated < pairQuota &&
                 !result.timedOut;
                 ++i) {
                for (std::size_t j = i + 1;
                     j < source.moves.size() &&
                     result.stats.splitPlansGenerated < splitQuota &&
                     result.stats.twoMoveCandidatesGenerated < pairQuota &&
                     !result.timedOut;
                     ++j) {
                    if (deadlineReached(deadline)) {
                        result.timedOut = true;
                        break;
                    }
                    const Move& first = source.moves[i].move;
                    const Move& second = source.moves[j].move;
                    if (first.destination == second.destination) {
                        continue;
                    }
                    if (static_cast<std::uint64_t>(first.count) +
                            static_cast<std::uint64_t>(second.count) >
                        source.sourcePopulation) {
                        continue;
                    }

                    TurnPlan plan;
                    plan.moves.push_back(first);
                    plan.moves.push_back(second);
                    if (!appendCombination(plan, CompoundPlanKind::Split)) {
                        return result;
                    }
                }
            }
        }
    }

    // Next generate explicit merge plans: distinct source groups converge
    // on the same destination. The transition engine aggregates all arrivals
    // before resolving a house, friendly group, or enemy encounter.
    const std::size_t mergeQuota = options.maxMergeCandidates;
    if (mergeQuota > 0 && pairQuota > 0 &&
        result.stats.twoMoveCandidatesGenerated < pairQuota &&
        !result.timedOut) {
        const std::vector<DestinationBucket> destinations =
            makeDestinationBuckets(
                board, actingSpecies, buckets, deadline, helperTimedOut
            );
        if (helperTimedOut) {
            result.timedOut = true;
            return result;
        }

        for (const DestinationBucket& destination : destinations) {
            if (result.stats.mergePlansGenerated >= mergeQuota ||
                result.stats.twoMoveCandidatesGenerated >= pairQuota ||
                result.timedOut) {
                break;
            }

            for (std::size_t i = 0;
                 i < destination.incoming.size() &&
                 result.stats.mergePlansGenerated < mergeQuota &&
                 result.stats.twoMoveCandidatesGenerated < pairQuota &&
                 !result.timedOut;
                 ++i) {
                for (std::size_t j = i + 1;
                     j < destination.incoming.size() &&
                     result.stats.mergePlansGenerated < mergeQuota &&
                     result.stats.twoMoveCandidatesGenerated < pairQuota &&
                     !result.timedOut;
                     ++j) {
                    const Move& first = destination.incoming[i].move;
                    const Move& second = destination.incoming[j].move;
                    if (first.source == second.source) {
                        continue;
                    }

                    TurnPlan plan;
                    plan.moves.push_back(first);
                    plan.moves.push_back(second);
                    if (!appendCombination(plan, CompoundPlanKind::Merge)) {
                        return result;
                    }
                }
            }
        }
    }

    // Fill the remaining pair budget with general plans drawn from distinct
    // sources and independently selected destinations.
    // Build pair plans by walking source-group offsets. This spreads the
    // earliest pairs across the board instead of exhausting all variants of
    // the first two source cells before considering other groups.
    if (pairQuota > 0 && buckets.size() >= 2) {
        for (std::size_t offset = 1;
             offset < buckets.size() &&
             result.stats.twoMoveCandidatesGenerated < pairQuota &&
             !result.timedOut;
             ++offset) {

            for (std::size_t first = 0;
                 first + offset < buckets.size() &&
                 result.stats.twoMoveCandidatesGenerated < pairQuota &&
                 !result.timedOut;
                 ++first) {

                const SourceBucket& a = buckets[first];
                const SourceBucket& b = buckets[first + offset];

                const std::size_t variants = std::min(
                    options.variantsPerSourceCombination,
                    std::max(a.moves.size(), b.moves.size())
                );

                for (std::size_t variant = 0;
                     variant < variants &&
                     result.stats.twoMoveCandidatesGenerated < pairQuota &&
                     !result.timedOut;
                     ++variant) {

                    TurnPlan plan;

                    plan.moves.push_back(
                        a.moves[selectedMoveIndex(a.moves.size(), variant)].move
                    );

                    plan.moves.push_back(
                        b.moves[selectedMoveIndex(b.moves.size(), variant)].move
                    );

                    if (!appendCombination(plan)) {
                        return result;
                    }
                }
            }
        }
    }

    // Three-source plans are generated only when requested. Their default
    // quota is deliberately smaller than the two-source quota.
    const std::size_t tripleQuota = options.maxThreeMoveCandidates;

    if (options.maxMovesPerPlan >= 3 &&
        tripleQuota > 0 &&
        buckets.size() >= 3 &&
        !result.timedOut) {

        for (std::size_t gap = 1;
             gap + 1 < buckets.size() &&
             result.stats.threeMoveCandidatesGenerated < tripleQuota &&
             !result.timedOut;
             ++gap) {

            for (std::size_t first = 0;
                 first + gap + 1 < buckets.size() &&
                 result.stats.threeMoveCandidatesGenerated < tripleQuota &&
                 !result.timedOut;
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
                     result.stats.threeMoveCandidatesGenerated < tripleQuota &&
                     !result.timedOut;
                     ++variant) {

                    TurnPlan plan;

                    plan.moves.push_back(
                        a.moves[selectedMoveIndex(a.moves.size(), variant)].move
                    );

                    plan.moves.push_back(
                        b.moves[selectedMoveIndex(b.moves.size(), variant)].move
                    );

                    plan.moves.push_back(
                        c.moves[selectedMoveIndex(c.moves.size(), variant)].move
                    );

                    if (!appendCombination(plan)) {
                        return result;
                    }
                }
            }
        }
    }

    // Four-source plans use four distinct source groups. The gap-based
    // enumeration spreads combinations across the available source groups
    // without constructing every possible four-group combination.
    const std::size_t quadQuota = options.maxFourMoveCandidates;

    if (options.maxMovesPerPlan >= 4 &&
        quadQuota > 0 &&
        buckets.size() >= 4 &&
        !result.timedOut) {

        for (std::size_t gap = 1;
             gap + 2 < buckets.size() &&
             result.stats.fourMoveCandidatesGenerated < quadQuota &&
             !result.timedOut;
             ++gap) {

            for (std::size_t first = 0;
                 first + gap + 2 < buckets.size() &&
                 result.stats.fourMoveCandidatesGenerated < quadQuota &&
                 !result.timedOut;
                 ++first) {

                const SourceBucket& a = buckets[first];
                const SourceBucket& b = buckets[first + gap];
                const SourceBucket& c = buckets[first + gap + 1];
                const SourceBucket& d = buckets[first + gap + 2];

                const std::size_t variants = std::min(
                    options.variantsPerSourceCombination,
                    std::max(
                        std::max(a.moves.size(), b.moves.size()),
                        std::max(c.moves.size(), d.moves.size())
                    )
                );

                for (std::size_t variant = 0;
                     variant < variants &&
                     result.stats.fourMoveCandidatesGenerated < quadQuota &&
                     !result.timedOut;
                     ++variant) {

                    TurnPlan plan;

                    plan.moves.push_back(
                        a.moves[selectedMoveIndex(a.moves.size(), variant)].move
                    );

                    plan.moves.push_back(
                        b.moves[selectedMoveIndex(b.moves.size(), variant)].move
                    );

                    plan.moves.push_back(
                        c.moves[selectedMoveIndex(c.moves.size(), variant)].move
                    );

                    plan.moves.push_back(
                        d.moves[selectedMoveIndex(d.moves.size(), variant)].move
                    );

                    if (!appendCombination(plan)) {
                        return result;
                    }
                }
            }
        }
    }

    result.timedOut = result.timedOut || deadlineReached(deadline);
    return result;
}

} // namespace vvw::ngs
