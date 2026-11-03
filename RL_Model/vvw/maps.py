"""Loading the server's XML maps.

Used for test fixtures and for local evaluation on the maps that shipped with
the assignment.  Training does not touch these files; it generates maps in
memory (P2).

    <Map Rows="5" Columns="10">
      <Humans X="2" Y="2" Count="4"/>
      <Werewolves X="4" Y="1" Count="4"/>
      <Vampires X="4" Y="3" Count="4"/>
    </Map>

Note the attribute order varies between the shipped files, so they are read by
name.  X is the column and Y is the row, which every shipped map confirms:
bigtest has Columns="40" with X up to 39 and Rows="50" with Y up to 49.
"""

from __future__ import annotations

import xml.etree.ElementTree as ElementTree
from pathlib import Path

from .state import GameState, VAMPIRES


def load_map(path: str | Path, me: str = VAMPIRES) -> GameState:
    root = ElementTree.parse(Path(path)).getroot()
    state = GameState(
        rows=int(root.attrib["Rows"]),
        columns=int(root.attrib["Columns"]),
        me=me,
    )

    for element in root:
        x = int(element.attrib["X"])
        y = int(element.attrib["Y"])
        count = int(element.attrib["Count"])
        humans = count if element.tag == "Humans" else 0
        vampires = count if element.tag == "Vampires" else 0
        werewolves = count if element.tag == "Werewolves" else 0
        state.set_cell((x, y), (humans, vampires, werewolves))
        if element.tag == "Humans":
            state.houses.add((x, y))

    state.assert_consistent()
    return state


#: The symmetries a fair map can be built on, as (name, transform factory).
SYMMETRIES = {
    "rotate180": lambda c, r: lambda x, y: (c - 1 - x, r - 1 - y),
    "flip_vertical": lambda c, r: lambda x, y: (x, r - 1 - y),
    "flip_horizontal": lambda c, r: lambda x, y: (c - 1 - x, y),
}


def fairness_symmetry(state: GameState) -> str | None:
    """Name a symmetry that makes the map fair, or None if there is none.

    "Humans are equidistant from the two starting positions" is realised in
    the shipped maps as a symmetry that swaps the two species and leaves the
    humans where they are.  Which symmetry differs: the generated maps
    (bigtest, newmap, testmap2) use a 180-degree rotation, while the hand-made
    thetrap.xml is mirrored about its horizontal axis.  So the invariant worth
    testing, and worth reproducing in the P2 generator, is "fair under some
    symmetry", not any one of them in particular.
    """
    for name, factory in SYMMETRIES.items():
        if _is_symmetric_under(state, factory(state.columns, state.rows)):
            return name
    return None


def _is_symmetric_under(state: GameState, transform) -> bool:
    from .state import VAMPIRES_IDX, WEREWOLVES_IDX

    for (x, y), (humans, vampires, werewolves) in state.cells.items():
        other = state.get(transform(x, y))
        if other[0] != humans:
            return False
        if other[VAMPIRES_IDX] != werewolves or other[WEREWOLVES_IDX] != vampires:
            return False
    return True
