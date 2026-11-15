#ifndef VVWS_NGS_OPPONENT_HISTORY_HPP
#define VVWS_NGS_OPPONENT_HISTORY_HPP

// Lightweight history of reconstructed board states.
// Header-only: no additional CMake source entry is required.
// C++11-compatible; uses only the C++ standard library.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vvw {
namespace history {

enum class Species {
    Vampires,
    Werewolves
};

// Copy the values needed by history from one cell in the live game state.
// Keep population fields wide until the server's maximum per-cell population
// is confirmed; uint32_t avoids silently truncating a large population.
struct HistoryCell {
    std::uint32_t humans;
    std::uint32_t vampires;
    std::uint32_t werewolves;
    bool is_house;

    HistoryCell()
        : humans(0), vampires(0), werewolves(0), is_house(false) {}

    HistoryCell(std::uint32_t h,
                std::uint32_t v,
                std::uint32_t w,
                bool house)
        : humans(h), vampires(v), werewolves(w), is_house(house) {}
};

struct HouseTrend {
    std::size_t cell_index;
    int oldest_distance;
    int newest_distance;
    double average_approach_per_turn; // Cells of Chebyshev distance closed per update.
    double confidence;                // 0..1: fraction of transitions showing approach.
    double risk;                      // 0..1: bounded approach strength * confidence.

    HouseTrend()
        : cell_index(0), oldest_distance(-1), newest_distance(-1),
          average_approach_per_turn(0.0), confidence(0.0), risk(0.0) {}
};

struct BoardSnapshot {
    std::size_t width;
    std::size_t height;
    std::uint32_t update_number;
    std::vector<HistoryCell> cells;

    BoardSnapshot() : width(0), height(0), update_number(0), cells() {}
};

class OpponentHistory {
public:
    static const std::size_t kCapacity = 4;

    OpponentHistory() : next_write_(0), count_(0) {}

    void clear() {
        for (std::size_t i = 0; i < kCapacity; ++i) {
            slots_[i] = BoardSnapshot();
        }
        next_write_ = 0;
        count_ = 0;
    }

    // Record a FULL reconstructed board after MAP or UPD has been applied.
    // The vector must contain width*height cells in row-major order:
    // index = y*width + x.
    void record(std::size_t width,
                std::size_t height,
                const std::vector<HistoryCell>& cells,
                std::uint32_t update_number) {
        if (width == 0 || height == 0 || cells.size() != width * height) {
            throw std::invalid_argument("OpponentHistory: invalid board dimensions/cell count");
        }

        BoardSnapshot snapshot;
        snapshot.width = width;
        snapshot.height = height;
        snapshot.update_number = update_number;
        snapshot.cells = cells;

        slots_[next_write_] = snapshot;
        next_write_ = (next_write_ + 1) % kCapacity;
        if (count_ < kCapacity) {
            ++count_;
        }
    }

    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    // age=0 is newest, age=1 is the snapshot immediately before it, etc.
    const BoardSnapshot& newest(std::size_t age = 0) const {
        if (age >= count_) {
            throw std::out_of_range("OpponentHistory: requested snapshot is not available");
        }
        const std::size_t index = (next_write_ + kCapacity - 1 - age) % kCapacity;
        return slots_[index];
    }

    // Estimate whether the nearest enemy force has been approaching each
    // currently active human house. This intentionally tracks regional
    // pressure, not the identity or exact path of an individual enemy group.
    // Need at least two snapshots. Returns one trend per active house.
    std::vector<HouseTrend> houseApproachTrends(Species opponent) const {
        std::vector<HouseTrend> result;
        if (count_ < 2) {
            return result;
        }

        const BoardSnapshot& current = newest(0);
        std::vector< std::vector<Position> > enemy_positions;
        enemy_positions.reserve(count_);

        // Gather enemy positions once per snapshot. This avoids rescanning
        // every board cell for every house.
        for (std::size_t chronological = 0; chronological < count_; ++chronological) {
            const std::size_t age = count_ - 1 - chronological;
            const BoardSnapshot& snapshot = newest(age);
            std::vector<Position> positions;
            for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
                const HistoryCell& cell = snapshot.cells[index];
                const std::uint32_t population =
                    (opponent == Species::Vampires) ? cell.vampires : cell.werewolves;
                if (population > 0) {
                    Position p;
                    p.x = static_cast<int>(index % snapshot.width);
                    p.y = static_cast<int>(index / snapshot.width);
                    positions.push_back(p);
                }
            }
            enemy_positions.push_back(positions);
        }

        const std::size_t transitions = count_ - 1;
        for (std::size_t house_index = 0; house_index < current.cells.size(); ++house_index) {
            const HistoryCell& house_cell = current.cells[house_index];
            if (!house_cell.is_house || house_cell.humans == 0) {
                continue;
            }

            const int house_x = static_cast<int>(house_index % current.width);
            const int house_y = static_cast<int>(house_index / current.width);
            std::vector<int> distances;
            distances.reserve(count_);
            bool all_snapshots_have_enemy = true;

            for (std::size_t s = 0; s < enemy_positions.size(); ++s) {
                int nearest = -1;
                for (std::size_t g = 0; g < enemy_positions[s].size(); ++g) {
                    const int dx = absInt(enemy_positions[s][g].x - house_x);
                    const int dy = absInt(enemy_positions[s][g].y - house_y);
                    const int distance = (dx > dy) ? dx : dy; // Chebyshev distance
                    if (nearest < 0 || distance < nearest) {
                        nearest = distance;
                    }
                }
                if (nearest < 0) {
                    all_snapshots_have_enemy = false;
                    break;
                }
                distances.push_back(nearest);
            }

            // A disappearance/reappearance makes the trend ambiguous.
            if (!all_snapshots_have_enemy || distances.size() != count_) {
                continue;
            }

            unsigned int approaching_transitions = 0;
            for (std::size_t i = 1; i < distances.size(); ++i) {
                if (distances[i] < distances[i - 1]) {
                    ++approaching_transitions;
                }
            }

            const int net_distance_closed = distances.front() - distances.back();
            const double positive_net = (net_distance_closed > 0)
                                            ? static_cast<double>(net_distance_closed)
                                            : 0.0;
            const double average_approach = positive_net / static_cast<double>(transitions);
            const double confidence = static_cast<double>(approaching_transitions) /
                                      static_cast<double>(transitions);

            // One move normally changes Chebyshev distance by at most one.
            // Clamp larger apparent changes (which may be caused by group
            // switching, combat, or ambiguity) rather than over-trusting them.
            const double strength = clamp01(average_approach);

            HouseTrend trend;
            trend.cell_index = house_index;
            trend.oldest_distance = distances.front();
            trend.newest_distance = distances.back();
            trend.average_approach_per_turn = average_approach;
            trend.confidence = confidence;
            trend.risk = strength * confidence;
            result.push_back(trend);
        }

        return result;
    }

    // Optional evaluator helper. Apply ONLY when the caller decides the house
    // is contested. Default maximum discount is 10%; this is a conservative
    // experimental starting value, not a validated game parameter.
    static double applyHouseHistoryDiscount(double base_house_value,
                                            double history_risk,
                                            bool contested,
                                            double max_discount = 0.10) {
        if (!contested || base_house_value <= 0.0) {
            return base_house_value;
        }
        const double safe_discount = clamp01(max_discount);
        const double safe_risk = clamp01(history_risk);
        return base_house_value * (1.0 - safe_discount * safe_risk);
    }

private:
    struct Position {
        int x;
        int y;
    };

    static int absInt(int value) { return value < 0 ? -value : value; }

    static double clamp01(double value) {
        if (value < 0.0) return 0.0;
        if (value > 1.0) return 1.0;
        return value;
    }

    std::array<BoardSnapshot, kCapacity> slots_;
    std::size_t next_write_;
    std::size_t count_;
};

} // namespace history
} // namespace vvw

#endif // VVWS_NGS_OPPONENT_HISTORY_HPP
