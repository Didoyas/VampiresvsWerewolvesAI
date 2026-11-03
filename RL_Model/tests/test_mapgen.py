"""Generated maps must be fair, legal, and acceptable to the server."""

import pytest

from vvw import mapgen
from vvw.maps import fairness_symmetry
from vvw.state import VAMPIRES, WEREWOLVES

SAMPLE = [0, 1, 2, 7, 42, 100_000, 200_000]


@pytest.mark.parametrize("seed", SAMPLE)
def test_generated_maps_are_consistent(seed):
    state = mapgen.generate(seed)
    state.assert_consistent()
    assert mapgen.MIN_SIDE <= state.rows <= mapgen.MAX_SIDE
    assert mapgen.MIN_SIDE <= state.columns <= mapgen.MAX_SIDE


@pytest.mark.parametrize("seed", SAMPLE)
def test_both_species_start_as_one_equal_group(seed):
    state = mapgen.generate(seed)
    assert len(state.groups(VAMPIRES)) == 1
    assert len(state.groups(WEREWOLVES)) == 1
    assert state.population(VAMPIRES) == state.population(WEREWOLVES)


@pytest.mark.parametrize("seed", SAMPLE)
def test_generated_maps_are_fair(seed):
    """Point symmetry is how the assignment's own generator makes them fair."""
    assert fairness_symmetry(mapgen.generate(seed)) == "rotate180"


@pytest.mark.parametrize("seed", SAMPLE)
def test_every_count_fits_in_a_byte(seed):
    state = mapgen.generate(seed)
    for contents in state.cells.values():
        for count in contents:
            assert 0 <= count <= 255


@pytest.mark.parametrize("seed", SAMPLE)
def test_population_stays_under_the_safe_cap(seed):
    """Until the server's behaviour above 255 is known, stay well below it."""
    state = mapgen.generate(seed)
    reachable = state.population(VAMPIRES) + state.human_population()
    assert reachable <= mapgen.SAFE_POPULATION_CAP


def test_the_same_seed_always_gives_the_same_map():
    first = mapgen.generate(123)
    second = mapgen.generate(123)
    assert first.cells == second.cells
    assert (first.rows, first.columns) == (second.rows, second.columns)


def test_different_seeds_give_different_maps():
    maps = {tuple(sorted(mapgen.generate(s).cells.items())) for s in range(20)}
    assert len(maps) > 15


def test_requested_dimensions_are_honoured():
    state = mapgen.generate(5, rows=10, columns=50)
    assert (state.rows, state.columns) == (10, 50)


@pytest.mark.parametrize("seed", SAMPLE)
def test_serialised_maps_carry_no_xml_comment(seed):
    """A comment anywhere in a map crashes the server before it listens."""
    xml = mapgen.to_xml(mapgen.generate(seed))
    assert "<!--" not in xml


def test_serialised_maps_round_trip(tmp_path):
    from vvw.maps import load_map

    original = mapgen.generate(77)
    path = mapgen.save(original, tmp_path / "generated.xml")
    reloaded = load_map(path)
    assert reloaded.cells == original.cells
    assert (reloaded.rows, reloaded.columns) == (original.rows, original.columns)


def test_splits_do_not_overlap():
    ranges = list(mapgen.SPLITS.values())
    for i, first in enumerate(ranges):
        for second in ranges[i + 1 :]:
            assert not (set(first) & set(second))


def test_asking_for_too_many_seeds_is_an_error():
    with pytest.raises(ValueError):
        mapgen.seeds("validation", len(mapgen.SPLITS["validation"]) + 1)


def test_unknown_split_is_an_error():
    with pytest.raises(ValueError):
        mapgen.seeds("nonesuch", 1)
