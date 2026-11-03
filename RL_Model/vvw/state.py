"""Board state.

The board is sparse: even a 50x50 map holds only a handful of occupied cells,
so `cells` maps a coordinate to its contents and omits empty cells entirely.

A cell holds (humans, vampires, werewolves).  In this game a cell never holds
two different kinds at once, but the wire format sends all three counts, so the
triple is kept as-is and `assert_consistent` checks the invariant.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Set, Tuple

Coord = Tuple[int, int]
Contents = Tuple[int, int, int]  # humans, vampires, werewolves

VAMPIRES = "vampires"
WEREWOLVES = "werewolves"

HUMANS_IDX = 0
VAMPIRES_IDX = 1
WEREWOLVES_IDX = 2


def species_index(species: str) -> int:
    if species == VAMPIRES:
        return VAMPIRES_IDX
    if species == WEREWOLVES:
        return WEREWOLVES_IDX
    raise ValueError(f"unknown species {species!r}")


def opponent_of(species: str) -> str:
    return WEREWOLVES if species == VAMPIRES else VAMPIRES


@dataclass
class GameState:
    rows: int = 0
    columns: int = 0
    houses: Set[Coord] = field(default_factory=set)
    cells: Dict[Coord, Contents] = field(default_factory=dict)
    me: str = VAMPIRES
    # Our starting cell, from HME; used to work out which species we are.
    home: Coord | None = None
    # Plies played by each side, so the move cap can be checked either way
    # while we do not yet know whether the server counts per player or in
    # total (see rules.Unknowns).
    plies: Dict[str, int] = field(default_factory=lambda: {VAMPIRES: 0, WEREWOLVES: 0})

    # ---- construction ----------------------------------------------------

    def copy(self) -> "GameState":
        return GameState(
            rows=self.rows,
            columns=self.columns,
            houses=self.houses,  # immutable in practice; never mutated in search
            cells=dict(self.cells),
            me=self.me,
            home=self.home,
            plies=dict(self.plies),
        )

    def reset_board(self) -> None:
        """Clear per-game data but keep nothing stale behind (END handling)."""
        self.rows = 0
        self.columns = 0
        self.houses = set()
        self.cells = {}
        self.home = None
        self.plies = {VAMPIRES: 0, WEREWOLVES: 0}

    # ---- queries ---------------------------------------------------------

    def in_bounds(self, x: int, y: int) -> bool:
        return 0 <= x < self.columns and 0 <= y < self.rows

    def get(self, coord: Coord) -> Contents:
        return self.cells.get(coord, (0, 0, 0))

    def set_cell(self, coord: Coord, contents: Contents) -> None:
        if contents == (0, 0, 0):
            self.cells.pop(coord, None)
        else:
            self.cells[coord] = contents

    def count_of(self, coord: Coord, species: str) -> int:
        return self.get(coord)[species_index(species)]

    def groups(self, species: str) -> List[Tuple[Coord, int]]:
        idx = species_index(species)
        return [(c, v[idx]) for c, v in self.cells.items() if v[idx] > 0]

    def population(self, species: str) -> int:
        idx = species_index(species)
        return sum(v[idx] for v in self.cells.values())

    def humans(self) -> List[Tuple[Coord, int]]:
        return [(c, v[HUMANS_IDX]) for c, v in self.cells.items() if v[HUMANS_IDX] > 0]

    def human_population(self) -> int:
        return sum(v[HUMANS_IDX] for v in self.cells.values())

    def neighbours(self, coord: Coord) -> Iterator[Coord]:
        """The eight adjacent cells that lie on the board."""
        x, y = coord
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                nx, ny = x + dx, y + dy
                if self.in_bounds(nx, ny):
                    yield (nx, ny)

    # ---- protocol updates ------------------------------------------------

    def apply_cell_updates(self, updates) -> None:
        """Apply MAP or UPD tuples.

        The server sends whole cell contents, not deltas, so a tuple with all
        three counts at zero empties the cell.  (`Unknowns.upd_is_absolute`
        records that this is still to be confirmed against the real server.)
        """
        for x, y, humans, vampires, werewolves in updates:
            self.set_cell((x, y), (humans, vampires, werewolves))

    def assert_consistent(self) -> None:
        for coord, (h, v, w) in self.cells.items():
            if not self.in_bounds(*coord):
                raise ValueError(f"cell {coord} is off the board")
            if min(h, v, w) < 0:
                raise ValueError(f"cell {coord} has a negative count: {(h, v, w)}")
            if sum(1 for n in (h, v, w) if n > 0) > 1:
                raise ValueError(f"cell {coord} mixes kinds: {(h, v, w)}")
