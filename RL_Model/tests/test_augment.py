"""Symmetry augmentation of training batches."""

import numpy as np
import pytest

from vvw import mapgen
from vvw.augment import (
    RECTANGLE_TRANSFORMS,
    SQUARE_TRANSFORMS,
    augment_batch,
    augment_planes,
    available_transforms,
    board_extent,
)
from vvw.features import encode_planes
from vvw.state import GameState, VAMPIRES


def planes_for(rows, columns, seed=3):
    return encode_planes(mapgen.generate(seed, rows=rows, columns=columns))


# ------------------------------------------------------------------- extent


@pytest.mark.parametrize("rows, columns", [(10, 10), (12, 20), (50, 10), (37, 43)])
def test_the_board_extent_is_recovered_from_the_marker_plane(rows, columns):
    assert board_extent(planes_for(rows, columns)) == (rows, columns)


def test_an_empty_board_does_not_break_augmentation():
    empty = np.zeros_like(planes_for(10, 10))
    assert augment_planes(empty, "rotate180").sum() == 0


# --------------------------------------------------------------- transforms


def test_a_rectangle_offers_only_the_four_orientation_preserving_transforms():
    assert available_transforms(12, 20) == list(RECTANGLE_TRANSFORMS)


def test_a_square_also_offers_the_transforms_that_swap_rows_and_columns():
    available = available_transforms(16, 16)
    assert set(SQUARE_TRANSFORMS) <= set(available)
    assert len(available) == 8


@pytest.mark.parametrize("name", RECTANGLE_TRANSFORMS)
def test_rectangle_transforms_keep_the_board_where_inference_expects_it(name):
    """The board must stay at the top-left of its padding.

    Rotating the whole padded array would move the board into another corner,
    a layout the environment never produces, so the network would be trained
    on positions it will never see.
    """
    original = planes_for(12, 20)
    transformed = augment_planes(original, name)
    assert board_extent(transformed) == (12, 20)
    assert not transformed[:, 12:, :].any()
    assert not transformed[:, :, 20:].any()


@pytest.mark.parametrize("name", RECTANGLE_TRANSFORMS + SQUARE_TRANSFORMS)
def test_no_transform_creates_or_destroys_population(name):
    original = planes_for(16, 16)
    assert augment_planes(original, name).sum() == pytest.approx(
        float(original.sum()), abs=1e-4
    )


@pytest.mark.parametrize("name", ["rotate180", "flip_vertical", "flip_horizontal"])
def test_every_mirror_is_its_own_inverse(name):
    original = planes_for(12, 20)
    assert np.allclose(augment_planes(augment_planes(original, name), name), original)


@pytest.mark.parametrize("name", ["rotate180", "flip_vertical", "flip_horizontal"])
def test_a_transform_actually_changes_the_board(name):
    original = planes_for(12, 20)
    assert not np.allclose(augment_planes(original, name), original)


def test_the_identity_transform_changes_nothing():
    original = planes_for(12, 20)
    assert np.allclose(augment_planes(original, "identity"), original)


def test_an_unknown_transform_is_an_error():
    with pytest.raises(ValueError):
        augment_planes(planes_for(10, 10), "cartwheel")


# ------------------------------------------------------------------- batches


def test_a_batch_is_augmented_in_place_and_keeps_its_shape():
    boards = np.stack([planes_for(12, 20, seed=s) for s in range(6)])
    out = augment_batch(boards, np.random.default_rng(0))
    assert out.shape == boards.shape
    for index in range(len(boards)):
        assert board_extent(out[index]) == (12, 20)
        assert out[index].sum() == pytest.approx(float(boards[index].sum()), abs=1e-4)


def test_a_batch_really_does_get_varied():
    # One board repeated; the augmented copies should not all be identical.
    boards = np.stack([planes_for(12, 20)] * 24)
    out = augment_batch(boards, np.random.default_rng(1))
    distinct = {out[i].tobytes() for i in range(len(out))}
    assert len(distinct) > 1


def test_mixed_board_sizes_are_each_augmented_legally():
    """Square boards may use all eight; rectangles must not get the other four."""
    boards = np.stack([planes_for(16, 16), planes_for(12, 20)])
    out = augment_batch(boards, np.random.default_rng(2))
    assert board_extent(out[0]) == (16, 16)
    assert board_extent(out[1]) == (12, 20)


def test_augmentation_leaves_the_label_meaningful():
    """Reorienting the board must not reorder the candidate list.

    The action is an index into that list, so if augmentation could permute
    it the supervised target would be wrong. Only `board` is touched.
    """
    from vvw.env import VvWEnv

    env = VvWEnv()
    observation, info = env.reset(seed=5)
    before_candidates = observation["candidates"].copy()
    before_mask = observation["mask"].copy()

    augmented = augment_planes(observation["board"], "rotate180")

    assert not np.allclose(augmented, observation["board"])
    assert np.allclose(observation["candidates"], before_candidates)
    assert np.array_equal(observation["mask"], before_mask)
    assert info["heuristic_choice"] == env.heuristic_choice
