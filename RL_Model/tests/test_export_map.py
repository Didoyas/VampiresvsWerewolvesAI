"""Exporting generated maps as server .xml files."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from export_map import DEFAULT_SIZES, to_xml, write_map

from vvw import mapgen
from vvw.maps import fairness_symmetry, load_map


def test_the_declaration_matches_the_shipped_files():
    """thetrap.xml and bigtest.xml both open with exactly this line, and the
    server's parser has already proved fragile once."""
    xml = to_xml(mapgen.generate(200_001, rows=10, columns=10))
    assert xml.startswith('<?xml version="1.0" encoding="utf-8" ?>\n')


def test_no_comments_anywhere():
    """A comment in the map file crashes the server outright."""
    assert "<!--" not in to_xml(mapgen.generate(200_002, rows=12, columns=15))


def test_elements_are_grouped_humans_then_werewolves_then_vampires():
    lines = to_xml(mapgen.generate(200_003, rows=20, columns=20)).splitlines()
    tags = [line.strip().split()[0].lstrip("<") for line in lines[2:-1]]
    assert tags == sorted(tags, key=["Humans", "Werewolves", "Vampires"].index)


def test_a_written_map_reloads_to_the_same_board(tmp_path):
    state = mapgen.generate(200_004, rows=25, columns=30)
    write_map(state, tmp_path / "m.xml")
    back = load_map(tmp_path / "m.xml")
    assert (back.rows, back.columns) == (state.rows, state.columns)
    assert {c: v for c, v in back.cells.items() if any(v)} == {
        c: v for c, v in state.cells.items() if any(v)
    }


def test_coordinates_survive_as_x_column_y_row(tmp_path):
    """The axis convention is the one thing a map file can get silently
    wrong: a transposed map still parses and still looks plausible."""
    state = mapgen.generate(200_005, rows=12, columns=40)
    write_map(state, tmp_path / "m.xml")
    back = load_map(tmp_path / "m.xml")
    assert back.columns == 40 and back.rows == 12
    for (x, y) in back.cells:
        assert 0 <= x < 40 and 0 <= y < 12


@pytest.mark.parametrize("name,seed,rows,columns", DEFAULT_SIZES)
def test_every_exported_map_is_fair(name, seed, rows, columns):
    """The server expects humans equidistant from both starts, which the
    shipped maps realise as a symmetry swapping the two species."""
    state = mapgen.generate(seed, rows=rows, columns=columns)
    assert fairness_symmetry(state) is not None, name


def test_an_unfair_board_is_refused_rather_than_written(tmp_path):
    from vvw.state import GameState

    lopsided = GameState(rows=10, columns=10)
    lopsided.set_cell((0, 0), (0, 5, 0))
    lopsided.set_cell((9, 9), (0, 0, 3))
    target = tmp_path / "bad.xml"
    with pytest.raises(ValueError, match="not fair"):
        write_map(lopsided, target)
    assert not target.exists()
