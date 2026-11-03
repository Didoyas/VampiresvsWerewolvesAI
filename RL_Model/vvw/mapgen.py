"""Generating fair maps for training and evaluation.

Built to match the family the assignment's own generator produces: symmetric
under a 180-degree rotation, with the two species on mirrored cells and equal
starting counts. That symmetry is how "humans are equidistant from the two
starting positions" shows up in the shipped files, and it guarantees neither
side has a structural advantage.

Two things the server forces on us:

  * **No XML comments.** Any `<!-- ... -->` in a map file makes the server's
    GUI throw InvalidCastException before it opens its socket.
  * Counts travel as single bytes, so no cell may exceed 255. Until the
    server's behaviour above that is known, total population per side is kept
    below `SAFE_POPULATION_CAP` as well.

Seeds are split into disjoint ranges so hyperparameters tuned on training maps
can be judged on maps they never saw.
"""

from __future__ import annotations

import random
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .state import Coord, GameState, VAMPIRES

MIN_SIDE = 10
MAX_SIDE = 50
SAFE_POPULATION_CAP = 250

#: Disjoint seed ranges. Tune on train, choose on validation, report on test.
SPLITS: Dict[str, range] = {
    "train": range(0, 100_000),
    "validation": range(100_000, 110_000),
    "test": range(200_000, 210_000),
}


def seeds(split: str, count: int) -> List[int]:
    """The first `count` seeds of a split."""
    if split not in SPLITS:
        raise ValueError(f"unknown split {split!r}; expected one of {sorted(SPLITS)}")
    available = SPLITS[split]
    if count > len(available):
        raise ValueError(f"{split} holds only {len(available)} seeds")
    return list(available[:count])


def generate(
    seed: int,
    rows: Optional[int] = None,
    columns: Optional[int] = None,
) -> GameState:
    """Build one fair map. The same seed always gives the same map."""
    rng = random.Random(seed)
    rows = rows or rng.randint(MIN_SIDE, MAX_SIDE)
    columns = columns or rng.randint(MIN_SIDE, MAX_SIDE)
    state = GameState(rows=rows, columns=columns, me=VAMPIRES)

    def mirror(cell: Coord) -> Coord:
        return (columns - 1 - cell[0], rows - 1 - cell[1])

    taken: set = set()

    # Starting cells: a mirrored pair, so never the self-mirrored centre.
    while True:
        home = (rng.randrange(columns), rng.randrange(rows))
        if mirror(home) != home:
            break
    away = mirror(home)
    start_count = rng.randint(2, 20)
    state.set_cell(home, (0, start_count, 0))
    state.set_cell(away, (0, 0, start_count))
    taken.update({home, away})

    # Houses, placed as mirrored pairs.
    area = rows * columns
    pairs = rng.randint(2, max(3, min(12, area // 150)))
    biggest_house = max(4, 2 * start_count)
    budget = SAFE_POPULATION_CAP - start_count

    for _ in range(pairs):
        cell = _free_cell(rng, rows, columns, taken, mirror)
        if cell is None:
            break
        count = rng.randint(1, biggest_house)
        twin = mirror(cell)
        if twin == cell:
            # The exact centre is its own mirror: one house, fair to both.
            if count > budget:
                break
            state.set_cell(cell, (count, 0, 0))
            state.houses.add(cell)
            taken.add(cell)
            budget -= count
            continue
        if count > budget:
            break
        state.set_cell(cell, (count, 0, 0))
        state.set_cell(twin, (count, 0, 0))
        state.houses.update({cell, twin})
        taken.update({cell, twin})
        budget -= count

    state.assert_consistent()
    return state


def _free_cell(
    rng: random.Random,
    rows: int,
    columns: int,
    taken: set,
    mirror,
    attempts: int = 60,
) -> Optional[Coord]:
    for _ in range(attempts):
        cell = (rng.randrange(columns), rng.randrange(rows))
        if cell not in taken and mirror(cell) not in taken:
            return cell
    return None


def to_xml(state: GameState) -> str:
    """Serialise in the server's format.

    Deliberately comment-free: a comment anywhere in the file crashes the
    server's GUI before it starts listening.
    """
    lines = [
        '<?xml version="1.0" encoding="utf-8" ?>',
        f'<Map Rows="{state.rows}" Columns="{state.columns}">',
    ]
    for (x, y), (humans, vampires, werewolves) in sorted(state.cells.items()):
        for tag, count in (
            ("Humans", humans),
            ("Vampires", vampires),
            ("Werewolves", werewolves),
        ):
            if count:
                lines.append(f'  <{tag} X="{x}" Y="{y}" Count="{count}"/>')
    lines.append("</Map>")
    return "\n".join(lines) + "\n"


def save(state: GameState, path: str | Path) -> Path:
    path = Path(path)
    path.write_text(to_xml(state), encoding="utf-8")
    return path
