"""Symmetry augmentation for training batches.

Taken from Chang Chia-Wei, "Size-Generalizable Reinforcement Learning for
m,n,k-Games Using Fully Convolutional Networks" (preprint, 2026), which
augments its training samples with rotations and reflections of the board.

It applies here for the same reason it applies there: nothing in the rules of
this game refers to an orientation. Reflect a position and the legal moves,
the combat thresholds and the winner all reflect with it, so a reflected
board is a genuinely different training sample describing the same strategic
situation. For a supervised warm start, which is bounded by how many labelled
positions we can afford to generate, that is up to four free samples for the
price of one -- eight on a square board.

Two details that matter here but not in the paper:

  * Our boards are rectangular, so a 90-degree rotation is only valid when
    rows == columns. The transforms are filtered per board.
  * A board sits at the top-left of a padded 50x50 array, so only the board
    region is transformed and it is written back in place. Rotating the whole
    padded array would move the board into a different corner, which is a
    configuration inference never produces.

The action label is untouched. Our action is an index into a candidate list,
and reorienting the board does not reorder that list, so the heuristic's pick
keeps the same index. The candidate features are population deltas and counts,
which have no orientation either.

**Measured result: this is a no-op for the current network, and it is
switched off.** A 50-iteration A/B on identical seeds gave accuracies of
0.256 / 0.569 / 0.344 / 0.494 / 0.331 without it and 0.256 / 0.562 / 0.344 /
0.494 / 0.331 with it. The reason is structural rather than accidental:
`PolicyValueNet` reduces the convolutional trunk with a global average pool,
and a global pool over spatial positions is invariant to any permutation of
them, rotations and reflections included. Feeding a reflected board through
it produces logits that differ by around 1e-7, which is floating-point noise.

So this architecture gets the game's symmetry for free, by construction,
where the paper has to buy it with four to eight times the training data. The
module is kept because it stops being a no-op the moment any head reads
spatial positions rather than a pooled summary.
"""

from __future__ import annotations

from typing import List

import numpy as np

#: Transforms valid on any rectangle: identity, 180 degrees, both mirrors.
RECTANGLE_TRANSFORMS = ("identity", "rotate180", "flip_vertical", "flip_horizontal")
#: Only on a square board, where swapping rows and columns is still the board.
SQUARE_TRANSFORMS = ("rotate90", "rotate270", "transpose", "anti_transpose")


def board_extent(planes: np.ndarray) -> tuple:
    """Recover (rows, columns) from the on-board marker plane.

    The marker is a solid rectangle at the top-left, so its first column and
    first row give the height and width directly.
    """
    marker = planes[4]
    rows = int(marker[:, 0].sum())
    columns = int(marker[0, :].sum())
    return rows, columns


def available_transforms(rows: int, columns: int) -> List[str]:
    if rows == columns:
        return list(RECTANGLE_TRANSFORMS) + list(SQUARE_TRANSFORMS)
    return list(RECTANGLE_TRANSFORMS)


def _transform_region(region: np.ndarray, name: str) -> np.ndarray:
    """`region` is (channels, rows, columns)."""
    if name == "identity":
        return region
    if name == "rotate180":
        return region[:, ::-1, ::-1]
    if name == "flip_vertical":
        return region[:, ::-1, :]
    if name == "flip_horizontal":
        return region[:, :, ::-1]
    if name == "rotate90":
        return np.rot90(region, k=1, axes=(1, 2))
    if name == "rotate270":
        return np.rot90(region, k=3, axes=(1, 2))
    if name == "transpose":
        return region.transpose(0, 2, 1)
    if name == "anti_transpose":
        return region[:, ::-1, ::-1].transpose(0, 2, 1)
    raise ValueError(f"unknown transform {name!r}")


def augment_planes(planes: np.ndarray, name: str) -> np.ndarray:
    """Reorient the board inside its padding, leaving the padding alone."""
    if name == "identity":
        return planes
    rows, columns = board_extent(planes)
    if rows == 0 or columns == 0:
        return planes

    out = np.zeros_like(planes)
    region = _transform_region(planes[:, :rows, :columns], name)
    out[:, : region.shape[1], : region.shape[2]] = region
    return out


def augment_batch(
    boards: np.ndarray, rng: np.random.Generator | None = None
) -> np.ndarray:
    """Independently reorient every board in a (N, C, H, W) batch."""
    rng = rng or np.random.default_rng()
    out = np.empty_like(boards)
    for index, planes in enumerate(boards):
        rows, columns = board_extent(planes)
        choices = available_transforms(rows, columns)
        out[index] = augment_planes(planes, str(rng.choice(choices)))
    return out
