
#include "../src/state_adapter.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace vvw::ngs;

namespace {

GameBoardView createTestView() {
    GameBoardView view;

    view.rows = 3;
    view.columns = 4;
    view.ourSpecies = Species::Vampires;

    // Twelve cells, stored in row-major order.
    view.cells.resize(12);

    // Cell at x=1, y=0: index = 0*4 + 1 = 1.
    view.cells[1] = {10, 20, 0, true};

    // Cell at x=2, y=1: index = 1*4 + 2 = 6.
    view.cells[6] = {5, 0, 15, false};

    // Cell at x=3, y=2: index = 2*4 + 3 = 11.
    view.cells[11] = {8, 4, 2, true};

    return view;
}

void testDimensionsAreCopied() {
    const GameBoardView view = createTestView();

    const BoardState board = adaptBoardState(view);

    assert(board.rows == 3);
    assert(board.columns == 4);
    assert(board.cells.size() == 12);
    assert(board.hasValidStorage());
}

void testSpeciesIsCopied() {
    const GameBoardView view = createTestView();

    const BoardState board = adaptBoardState(view);

    assert(board.ourSpecies == Species::Vampires);
}

void testPopulationValuesAreCopied() {
    const GameBoardView view = createTestView();

    const BoardState board = adaptBoardState(view);

    const Cell& cell = board.at({2, 1});

    assert(cell.humans == 5);
    assert(cell.vampires == 0);
    assert(cell.werewolves == 15);
}

void testHouseFlagsAreCopied() {
    const GameBoardView view = createTestView();

    const BoardState board = adaptBoardState(view);

    assert(board.at({1, 0}).isHouse);
    assert(!board.at({2, 1}).isHouse);
    assert(board.at({3, 2}).isHouse);
}

void testRowMajorOrderingIsPreserved() {
    const GameBoardView view = createTestView();

    const BoardState board = adaptBoardState(view);

    // Index 11 corresponds to x=3, y=2 on a 4-column board.
    assert(board.cells[11].humans == 8);
    assert(board.cells[11].vampires == 4);
    assert(board.cells[11].werewolves == 2);
    assert(board.cells[11].isHouse);
}

void testPreviousPlyFootprintIsCopied() {
    const GameBoardView view = createTestView();

    PlyFootprint previous;

    previous.known = true;
    previous.sources.push_back({0, 0});
    previous.destinations.push_back({1, 0});

    const BoardState board =
        adaptBoardState(view, previous);

    assert(board.previousPly.known);
    assert(board.previousPly.wasSource({0, 0}));
    assert(board.previousPly.wasDestination({1, 0}));
}

void testUnknownFootprintRemainsUnknown() {
    const GameBoardView view = createTestView();

    const BoardState board = adaptBoardState(view);

    // The adapter must not invent previous movement information.
    assert(!board.previousPly.known);
    assert(board.previousPly.sources.empty());
    assert(board.previousPly.destinations.empty());
}

void testGameHorizonIsCopied() {
    const GameBoardView view = createTestView();

    GameHorizon horizon;

    horizon.ourPliesUsed = 10;
    horizon.opponentPliesUsed = 8;
    horizon.elapsedSeconds = 45.0;

    const BoardState board =
        adaptBoardState(view, PlyFootprint{}, horizon);

    assert(board.horizon.ourPliesUsed == 10);
    assert(board.horizon.opponentPliesUsed == 8);
    assert(board.horizon.elapsedSeconds == 45.0);
}

void testEmptyDimensionsAreRejected() {
    GameBoardView view;

    view.rows = 0;
    view.columns = 4;

    bool threw = false;

    try {
        (void)adaptBoardState(view);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
}

void testIncorrectCellCountIsRejected() {
    GameBoardView view = createTestView();

    // The board needs 3 * 4 = 12 cells.
    view.cells.pop_back();

    bool threw = false;

    try {
        (void)adaptBoardState(view);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
}

void testOutOfBoundsPreviousFootprintIsRejected() {
    const GameBoardView view = createTestView();

    PlyFootprint previous;

    previous.known = true;
    previous.sources.push_back({10, 10});

    bool threw = false;

    try {
        (void)adaptBoardState(view, previous);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
}

} // namespace

int main() {
    testDimensionsAreCopied();
    testSpeciesIsCopied();
    testPopulationValuesAreCopied();
    testHouseFlagsAreCopied();
    testRowMajorOrderingIsPreserved();

    testPreviousPlyFootprintIsCopied();
    testUnknownFootprintRemainsUnknown();
    testGameHorizonIsCopied();

    testEmptyDimensionsAreRejected();
    testIncorrectCellCountIsRejected();
    testOutOfBoundsPreviousFootprintIsRejected();

    std::cout << "All state adapter tests passed.\n";

    return 0;
}