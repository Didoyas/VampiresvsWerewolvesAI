#include "candidate_generator.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace vvw::ngs {
namespace {

constexpr Population kMaxWireMovementCount = 255;

bool isKnownSpecies(Species species) noexcept {
    return species == Species::Vampires ||
           species == Species::Werewolves;
}

Population populationFor(const Cell& cell, Species species) noexcept {
    if (species == Species::Vampires) {
        return cell.vampires;
    }
    if (species == Species::Werewolves) {
        return cell.werewolves;
    }
    return 0;
}

bool areSplitRatiosValid(
    const std::vector<SplitRatio>& ratios
) noexcept {
    if (ratios.empty()) {
        return false;
    }

    for (const SplitRatio& ratio : ratios) {
        if (ratio.denominator == 0 ||
            ratio.numerator == 0 ||
            ratio.numerator > ratio.denominator) {
            return false;
        }
    }

    return true;
}

// Compute ceil(population * numerator / denominator) without floating-point
// arithmetic. The product of two uint32_t values fits in uint64_t.
Population splitPopulation(
    Population population,
    const SplitRatio& ratio
) noexcept {
    const std::uint64_t product =
        static_cast<std::uint64_t>(population) * ratio.numerator;
    const std::uint64_t denominator = ratio.denominator;

    std::uint64_t count = product / denominator;
    if (product % denominator != 0) {
        ++count;
    }

    count = std::min(
        count,
        static_cast<std::uint64_t>(kMaxWireMovementCount)
    );

    return static_cast<Population>(count);
}

// Deduplicate rounded/capped split counts while preserving configured order.
std::vector<Population> makeUniqueSplitCounts(
    Population population,
    const std::vector<SplitRatio>& ratios,
    std::size_t& duplicateCount
) {
    std::vector<Population> counts;
    counts.reserve(ratios.size());

    for (const SplitRatio& ratio : ratios) {
        const Population count = splitPopulation(population, ratio);
        const bool alreadyPresent =
            std::find(counts.begin(), counts.end(), count) != counts.end();

        if (alreadyPresent) {
            ++duplicateCount;
        } else {
            counts.push_back(count);
        }
    }

    return counts;
}

CandidateGenerationResult failure(CandidateGenerationError error) {
    CandidateGenerationResult result;
    result.error = error;
    return result;
}

} // namespace

CandidateGenerationResult generateSingleMoveCandidates(
    const BoardState& board,
    Species actingSpecies,
    const CandidateGenerationOptions& options,
    SearchDeadline deadline
) {
    if (!board.hasValidStorage()) {
        return failure(CandidateGenerationError::InvalidBoard);
    }

    if (!isKnownSpecies(actingSpecies)) {
        return failure(CandidateGenerationError::InvalidSpecies);
    }

    if (!areSplitRatiosValid(options.splitRatios)) {
        return failure(CandidateGenerationError::InvalidSplitRatio);
    }

    if (options.requirePreviousPlyFootprint &&
        !board.previousPly.known) {
        return failure(
            CandidateGenerationError::MissingPreviousPlyFootprint
        );
    }

    ValidationOptions validationOptions;
    validationOptions.requirePreviousPlyFootprint =
        options.requirePreviousPlyFootprint;

    CandidateGenerationResult result;

    // Stable row-major source ordering and stable clockwise-style neighbour
    // ordering make candidate generation deterministic and reproducible.
    for (int y = 0; y < board.rows && !result.timedOut; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            if (deadlineReached(deadline)) {
                result.timedOut = true;
                break;
            }

            const Position source{x, y};
            const Population sourcePopulation =
                populationFor(board.at(source), actingSpecies);

            if (sourcePopulation == 0) {
                continue;
            }

            ++result.stats.sourceGroupsExamined;

            const std::vector<Population> splitCounts =
                makeUniqueSplitCounts(
                    sourcePopulation,
                    options.splitRatios,
                    result.stats.duplicateCountsSkipped
                );

            for (int dy = -1; dy <= 1 && !result.timedOut; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (deadlineReached(deadline)) {
                        result.timedOut = true;
                        break;
                    }

                    if (dx == 0 && dy == 0) {
                        continue;
                    }

                    const Position destination{x + dx, y + dy};
                    if (!board.inBounds(destination)) {
                        continue;
                    }

                    ++result.stats.neighbourCellsExamined;

                    for (const Population count : splitCounts) {
                        if (deadlineReached(deadline)) {
                            result.timedOut = true;
                            break;
                        }

                        TurnPlan plan;
                        plan.moves.push_back({source, count, destination});

                        const ValidationResult validation =
                            validateTurnPlan(
                                board,
                                plan,
                                actingSpecies,
                                validationOptions
                            );

                        if (validation.valid()) {
                            result.candidates.push_back(std::move(plan));
                            ++result.stats.candidatesGenerated;
                        } else {
                            ++result.stats.candidatesRejectedByValidator;
                        }
                    }
                }
                if (result.timedOut) {
                    break;
                }
            }
        }
    }

    result.timedOut = result.timedOut || deadlineReached(deadline);
    return result;
}

} // namespace vvw::ngs
