
#pragma once

#include "types.hpp"

namespace vvw::ngs {

// A copy of one cell from the client's live GameState.
// This is deliberately separate from the NGS Cell type.
struct GameCellView {
    Population humans{0};
    Population vampires{0};
    Population werewolves{0};
    bool isHouse{false};
};

// A read-only snapshot supplied by the TCP client.
struct GameBoardView {
    int rows{0};
    int columns{0};

    Species ourSpecies{Species::Unknown};

    // Row-major layout: index = y * columns + x.
    std::vector<GameCellView> cells;
};

// Convert the server board representation into our NGS representation.
//
// Previous-ply information is supplied separately because a board snapshot
// alone cannot reliably reconstruct every previous movement.
BoardState adaptBoardState(
    const GameBoardView& source,
    const PlyFootprint& previousPly = {},
    const GameHorizon& horizon = {}
);

} // namespace vvw::ngs