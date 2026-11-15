
#include "state_adapter.hpp"

#include <limits>
#include <stdexcept>

namespace vvw::ngs {
namespace {

void validateDimensions(const GameBoardView& source) {
    if (source.rows <= 0 || source.columns <= 0) {
        throw std::invalid_argument(
            "StateAdapter: board dimensions must be positive");
    }

    const std::size_t rows =
        static_cast<std::size_t>(source.rows);

    const std::size_t columns =
        static_cast<std::size_t>(source.columns);

    if (rows > std::numeric_limits<std::size_t>::max() / columns) {
        throw std::invalid_argument(
            "StateAdapter: board dimensions overflow");
    }

    if (source.cells.size() != rows * columns) {
        throw std::invalid_argument(
            "StateAdapter: cell count does not match board dimensions");
    }
}

void validateFootprint(
    const PlyFootprint& footprint,
    int rows,
    int columns
) {
    if (!footprint.known) {
        return;
    }

    const auto isInside = [rows, columns](Position p) {
        return p.x >= 0 &&
               p.y >= 0 &&
               p.x < columns &&
               p.y < rows;
    };

    for (Position p : footprint.sources) {
        if (!isInside(p)) {
            throw std::invalid_argument(
                "StateAdapter: previous source is outside the board");
        }
    }

    for (Position p : footprint.destinations) {
        if (!isInside(p)) {
            throw std::invalid_argument(
                "StateAdapter: previous destination is outside the board");
        }
    }
}

} // namespace


BoardState adaptBoardState(
    const GameBoardView& source,
    const PlyFootprint& previousPly,
    const GameHorizon& horizon
) {
    validateDimensions(source);

    validateFootprint(
        previousPly,
        source.rows,
        source.columns
    );

    BoardState result;

    result.rows = source.rows;
    result.columns = source.columns;
    result.ourSpecies = source.ourSpecies;

    result.previousPly = previousPly;
    result.horizon = horizon;

    result.cells.reserve(source.cells.size());

    // Preserve row-major ordering.
    for (const GameCellView& sourceCell : source.cells) {
        Cell destinationCell;

        destinationCell.humans = sourceCell.humans;
        destinationCell.vampires = sourceCell.vampires;
        destinationCell.werewolves = sourceCell.werewolves;
        destinationCell.isHouse = sourceCell.isHouse;

        result.cells.push_back(destinationCell);
    }

    return result;
}

} // namespace vvw::ngs