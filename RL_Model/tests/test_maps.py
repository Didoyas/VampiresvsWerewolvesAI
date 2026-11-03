"""Reading the maps that shipped with the assignment."""

from pathlib import Path

import pytest

from vvw.maps import fairness_symmetry, load_map
from vvw.state import VAMPIRES, WEREWOLVES

ASSIGNMENT_ROOT = Path(__file__).resolve().parents[2]
MAPS = {
    "thetrap": ASSIGNMENT_ROOT / "thetrap.xml",
    "bigtest": ASSIGNMENT_ROOT / "bigtest.xml",
    "newmap": ASSIGNMENT_ROOT / "Map generator" / "newmap.xml",
    "testmap2": ASSIGNMENT_ROOT / "Map generator" / "testmap2.xml",
}
AVAILABLE = {name: path for name, path in MAPS.items() if path.exists()}


@pytest.mark.parametrize("name", sorted(AVAILABLE))
def test_every_shipped_map_loads_and_is_consistent(name):
    state = load_map(AVAILABLE[name])
    state.assert_consistent()
    assert state.rows > 0 and state.columns > 0
    assert state.population(VAMPIRES) > 0
    assert state.population(WEREWOLVES) > 0


@pytest.mark.parametrize("name", sorted(AVAILABLE))
def test_coordinates_fit_the_declared_dimensions(name):
    """x is the column and y is the row, not the other way round."""
    state = load_map(AVAILABLE[name])
    for x, y in state.cells:
        assert 0 <= x < state.columns, f"x={x} exceeds {state.columns} columns"
        assert 0 <= y < state.rows, f"y={y} exceeds {state.rows} rows"


@pytest.mark.parametrize("name", sorted(AVAILABLE))
def test_every_shipped_map_is_fair_under_some_symmetry(name):
    """This is what "equidistant from both starting positions" means here."""
    assert fairness_symmetry(load_map(AVAILABLE[name])) is not None


@pytest.mark.parametrize(
    "name, symmetry",
    [
        ("bigtest", "rotate180"),
        ("newmap", "rotate180"),
        ("testmap2", "rotate180"),
        ("thetrap", "flip_vertical"),
    ],
)
def test_which_symmetry_each_map_uses(name, symmetry):
    """The generated maps rotate; the hand-made thetrap.xml mirrors instead.

    Recorded so the P2 generator reproduces the generated-map family rather
    than assuming one single symmetry covers every board we will meet.
    """
    if name not in AVAILABLE:
        pytest.skip(f"{name}.xml not present")
    assert fairness_symmetry(load_map(AVAILABLE[name])) == symmetry


@pytest.mark.skipif("thetrap" not in AVAILABLE, reason="thetrap.xml not present")
def test_thetrap_contents():
    state = load_map(MAPS["thetrap"])
    assert (state.rows, state.columns) == (5, 10)
    assert state.get((4, 1)) == (0, 0, 4)  # werewolves
    assert state.get((4, 3)) == (0, 4, 0)  # vampires
    assert state.houses == {(2, 2), (9, 0), (9, 2), (9, 4)}
