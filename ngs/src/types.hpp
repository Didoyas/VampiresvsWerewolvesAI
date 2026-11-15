
#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace vvw::ngs {

using Population = std::uint32_t;

struct Position {
    int x{0};
    int y{0};
};

inline bool operator==(const Position& lhs, const Position& rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

inline bool operator!=(const Position& lhs, const Position& rhs) noexcept {
    return !(lhs == rhs);
}

enum class Species {
    Unknown,
    Vampires,
    Werewolves
};

inline Species opponentOf(Species species) noexcept {
    switch (species) {
        case Species::Vampires:
            return Species::Werewolves;
        case Species::Werewolves:
            return Species::Vampires;
        case Species::Unknown:
            return Species::Unknown;
    }
    return Species::Unknown;
}

struct Cell {
    Population humans{0};
    Population vampires{0};
    Population werewolves{0};
    bool isHouse{false};
};

struct Move {
    Position source{};
    Population count{0};
    Position destination{};
};

struct TurnPlan {
    std::vector<Move> moves;
};

struct PlyFootprint {
    bool known{false};
    std::vector<Position> sources;
    std::vector<Position> destinations;

    bool wasSource(Position position) const noexcept {
        return std::find(sources.begin(), sources.end(), position)
               != sources.end();
    }

    bool wasDestination(Position position) const noexcept {
        return std::find(destinations.begin(), destinations.end(), position)
               != destinations.end();
    }
};

struct GameHorizon {
    static constexpr std::uint16_t kMaxPliesPerSide = 100;
    static constexpr double kMaxGameSeconds = 300.0;

    std::uint16_t ourPliesUsed{0};
    std::uint16_t opponentPliesUsed{0};
    double elapsedSeconds{0.0};

    std::uint16_t ourPliesRemaining() const noexcept {
        return ourPliesUsed >= kMaxPliesPerSide
            ? 0
            : static_cast<std::uint16_t>(
                  kMaxPliesPerSide - ourPliesUsed);
    }

    std::uint16_t opponentPliesRemaining() const noexcept {
        return opponentPliesUsed >= kMaxPliesPerSide
            ? 0
            : static_cast<std::uint16_t>(
                  kMaxPliesPerSide - opponentPliesUsed);
    }

    double secondsRemaining() const noexcept {
        return elapsedSeconds >= kMaxGameSeconds
            ? 0.0
            : kMaxGameSeconds - std::max(0.0, elapsedSeconds);
    }

    bool isTerminalByLimit() const noexcept {
        return ourPliesRemaining() == 0 ||
               opponentPliesRemaining() == 0 ||
               secondsRemaining() <= 0.0;
    }
};

struct BoardState {
    int rows{0};
    int columns{0};

    std::vector<Cell> cells;

    Species ourSpecies{Species::Unknown};

    PlyFootprint previousPly;
    GameHorizon horizon;

    bool inBounds(Position position) const noexcept {
        return position.x >= 0 &&
               position.y >= 0 &&
               position.x < columns &&
               position.y < rows;
    }

    std::size_t index(Position position) const {
        if (!inBounds(position)) {
            throw std::out_of_range(
                "BoardState position is outside the board");
        }

        return static_cast<std::size_t>(position.y) *
                   static_cast<std::size_t>(columns) +
               static_cast<std::size_t>(position.x);
    }

    const Cell& at(Position position) const {
        return cells.at(index(position));
    }

    Cell& at(Position position) {
        return cells.at(index(position));
    }

    bool hasValidStorage() const noexcept {
        return rows > 0 &&
               columns > 0 &&
               cells.size() ==
                   static_cast<std::size_t>(rows) *
                   static_cast<std::size_t>(columns);
    }
};

} // namespace vvw::ngs