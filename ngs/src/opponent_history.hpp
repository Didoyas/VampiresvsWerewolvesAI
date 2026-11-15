
#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace vvw::ngs {

// Compact copy of the information needed for historical analysis.
// Population values remain uint32_t to avoid unnecessary truncation.
struct HistoryCell {
    std::uint32_t humans{0};
    std::uint32_t vampires{0};
    std::uint32_t werewolves{0};
    bool is_house{false};

    HistoryCell() = default;

    HistoryCell(
        std::uint32_t h,
        std::uint32_t v,
        std::uint32_t w,
        bool house
    )
        : humans(h),
          vampires(v),
          werewolves(w),
          is_house(house) {
    }
};

// One complete reconstructed board observation.
struct BoardSnapshot {
    std::size_t width{0};
    std::size_t height{0};

    // Both fields contain the same chronological identifier.
    // update_number preserves the original API.
    std::uint32_t update_number{0};
    std::uint32_t turn{0};

    std::vector<HistoryCell> cells;
};

// Historical movement explicitly confirmed by the protocol.
struct OpponentMovementObservation {
    std::uint32_t turn{0};
    Position source{};
    Position destination{};
    Population count{0};
};

// Original house approach analysis.
struct HouseTrend {
    std::size_t cell_index{0};

    int oldest_distance{-1};
    int newest_distance{-1};

    double average_approach_per_turn{0.0};
    double confidence{0.0};
    double risk{0.0};
};

// Historical occupancy statistics for one position.
struct OpponentPositionStatistics {
    std::size_t snapshotsConsidered{0};
    std::size_t presenceCount{0};

    double presenceFrequency{0.0};

    double recencyWeightedPresence{0.0};
    double recencyWeightedPresenceFrequency{0.0};

    Population latestPopulation{0};
    Population maximumObservedPopulation{0};

    bool hasBeenObserved{false};
    std::uint32_t lastSeenTurn{0};
};

enum class OpponentHistoryError {
    None,
    InvalidConfiguration,
    InvalidBoard,
    InvalidSpecies,
    InvalidTurn,
    BoardDimensionsChanged,
    HistoryNotInitialized,
    InvalidPosition,
    InvalidMovement,
    InvalidHalfLife
};

struct HistoryUpdateResult {
    OpponentHistoryError error{OpponentHistoryError::None};

    std::size_t positionsObserved{0};
    std::size_t snapshotsRetained{0};

    bool valid() const noexcept {
        return error == OpponentHistoryError::None;
    }
};

struct PositionStatisticsResult {
    OpponentHistoryError error{OpponentHistoryError::None};

    OpponentPositionStatistics statistics{};

    bool valid() const noexcept {
        return error == OpponentHistoryError::None;
    }
};

class OpponentHistory {
public:
    // Default retention accommodates the expected game history.
    static constexpr std::size_t kCapacity = 256;

    explicit OpponentHistory(
        Species opponentSpecies = Species::Unknown,
        std::size_t maxSnapshots = kCapacity
    );

    bool isConfigured() const noexcept;

    Species opponentSpecies() const noexcept;

    // ---------------------------------------------------------
    // Original snapshot-management API
    // ---------------------------------------------------------

    void clear();

    // Records a complete reconstructed board.
    //
    // Cells must be row-major:
    // index = y * width + x.
    //
    // Throws std::invalid_argument for invalid dimensions,
    // changed dimensions, or non-increasing update numbers.
    void record(
        std::size_t width,
        std::size_t height,
        const std::vector<HistoryCell>& cells,
        std::uint32_t update_number
    );

    // Records directly from the existing NGS BoardState.
    HistoryUpdateResult recordObservation(
        const BoardState& board,
        std::uint32_t turn
    );

    std::size_t size() const noexcept;

    bool empty() const noexcept;

    std::size_t capacity() const noexcept;

    // age=0: newest snapshot.
    // age=1: immediately preceding snapshot.
    const BoardSnapshot& newest(std::size_t age = 0) const;

    // Read retained snapshots in chronological order.
    const std::vector<BoardSnapshot>& snapshots() const noexcept;

    // ---------------------------------------------------------
    // Original house approach analysis
    // ---------------------------------------------------------

    // Estimates whether the nearest opponent force has been
    // approaching currently active human houses.
    //
    // Requires at least two snapshots.
    // Returns one trend per active human house.
    std::vector<HouseTrend> houseApproachTrends(
        Species opponent
    ) const;

    // Optional evaluator helper retained from the original code.
    static double applyHouseHistoryDiscount(
        double base_house_value,
        double history_risk,
        bool contested,
        double max_discount = 0.10
    ) noexcept;

    // ---------------------------------------------------------
    // New confirmed movement history
    // ---------------------------------------------------------

    OpponentHistoryError recordConfirmedMovement(
        std::uint32_t turn,
        Position source,
        Position destination,
        Population count
    );

    const std::vector<OpponentMovementObservation>&
    confirmedMovements() const noexcept;

    // ---------------------------------------------------------
    // New position statistics
    // ---------------------------------------------------------

    PositionStatisticsResult getPositionStatistics(
        Position position,
        std::uint32_t currentTurn,
        double halfLifeTurns = 8.0
    ) const;

private:
    Species opponentSpecies_{Species::Unknown};

    std::size_t maxSnapshots_{kCapacity};

    int rows_{0};
    int columns_{0};

    bool dimensionsInitialized_{false};

    bool hasMovementTurn_{false};
    std::uint32_t latestMovementTurn_{0};

    // Kept in chronological order: oldest to newest.
    std::vector<BoardSnapshot> snapshots_;

    std::vector<OpponentMovementObservation> confirmedMovements_;

    bool inBounds(Position position) const noexcept;

    bool isAdjacent(
        Position source,
        Position destination
    ) const noexcept;

    static int absInt(int value) noexcept;

    static double clamp01(double value) noexcept;

    static Population populationFor(
        const HistoryCell& cell,
        Species species
    ) noexcept;

    static bool isPlayableSpecies(Species species) noexcept;
};

} // namespace vvw::ngs
