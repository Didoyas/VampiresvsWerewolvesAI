"""Write generated maps out as server .xml files.

    python tools/export_map.py --out maps_generated

Training generates maps in memory and never touches disk, so there has never
been a way to look at one in the real game. These files load straight into
the assignment's own server, which is the only faithful way to watch a
checkpoint play: the server deals the moves, rolls the battles and enforces
the move cap, and our simulator does not get a vote.

Every file is checked before it is written:

  * it round-trips -- reading it back gives the same board we generated;
  * it is fair under one of the symmetries in `vvw.maps.SYMMETRIES`, the
    property the shipped maps have and the server expects.

The shipped maps use different symmetries (bigtest, newmap and testmap2 are
rotations; thetrap is a mirror), so "fair under some symmetry" is the real
invariant rather than any one of them.

X is the column and Y is the row, as `vvw.maps` documents and every shipped
map confirms.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from vvw import mapgen
from vvw.maps import fairness_symmetry, load_map
from vvw.state import GameState

#: Sizes worth having on disk: the two ends of the tournament range and a
#: couple in between, so a checkpoint can be watched on a board it trained
#: on early and one it only saw late in the curriculum.
DEFAULT_SIZES: List[Tuple[str, int, int, int]] = [
    # name,        seed,  rows, columns
    ("small_10x10", 200_001, 10, 10),
    ("small_12x15", 200_002, 12, 15),
    ("mid_20x20", 200_003, 20, 20),
    ("mid_25x30", 200_004, 25, 30),
    ("large_40x40", 200_005, 40, 40),
    ("large_50x50", 200_006, 50, 50),
]


def to_xml(state: GameState) -> str:
    """Byte-for-byte in the style of the shipped maps.

    Written by hand rather than through `ElementTree.tostring` because the
    server's parser has already proved fragile once -- it crashes outright on
    an XML comment -- so this is not the place to introduce a different
    declaration, a different self-closing style or a different element order
    and then wonder which one it disliked.

    The shipped files put every Humans element first, then Werewolves, then
    Vampires; both bigtest.xml and thetrap.xml agree on that, and on
    X-then-Y-then-Count inside each element.
    """
    lines = [
        '<?xml version="1.0" encoding="utf-8" ?>',
        f'<Map Rows="{state.rows}" Columns="{state.columns}">',
    ]
    buckets: Dict[str, List[str]] = {"Humans": [], "Werewolves": [], "Vampires": []}
    for (x, y), (humans, vampires, werewolves) in sorted(state.cells.items()):
        for tag, count in (
            ("Humans", humans),
            ("Werewolves", werewolves),
            ("Vampires", vampires),
        ):
            if count:
                buckets[tag].append(
                    f'  <{tag} X="{x}" Y="{y}" Count="{count}"/>'
                )
    for tag in ("Humans", "Werewolves", "Vampires"):
        lines.extend(buckets[tag])
    lines.append("</Map>")
    return "\n".join(lines) + "\n"


def write_map(state: GameState, path: Path) -> str:
    """Write it, then read it back and check it survived."""
    symmetry = fairness_symmetry(state)
    if symmetry is None:
        raise ValueError(f"{path.name} is not fair under any known symmetry")

    path.write_text(to_xml(state), encoding="utf-8")

    reloaded = load_map(path)
    if (reloaded.rows, reloaded.columns) != (state.rows, state.columns):
        raise ValueError(f"{path.name} changed size on the round trip")
    if {c: v for c, v in reloaded.cells.items() if any(v)} != {
        c: v for c, v in state.cells.items() if any(v)
    }:
        raise ValueError(f"{path.name} changed contents on the round trip")
    return symmetry


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="maps_generated")
    parser.add_argument(
        "--seeds",
        type=int,
        nargs="*",
        help="extra seeds to export at the generator's own random size",
    )
    args = parser.parse_args(argv)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    written = []
    for name, seed, rows, columns in DEFAULT_SIZES:
        state = mapgen.generate(seed, rows=rows, columns=columns)
        symmetry = write_map(state, out / f"{name}.xml")
        written.append((f"{name}.xml", state, symmetry))
    for seed in args.seeds or []:
        state = mapgen.generate(seed)
        name = f"seed{seed}_{state.rows}x{state.columns}.xml"
        symmetry = write_map(state, out / name)
        written.append((name, state, symmetry))

    print(f"{len(written)} maps written to {out.resolve()}\n")
    print(f"{'file':<22}{'size':>9}{'houses':>8}{'humans':>8}{'start':>7}  symmetry")
    for name, state, symmetry in written:
        print(
            f"{name:<22}{state.rows}x{state.columns:<6}{len(state.houses):>8}"
            f"{state.human_population():>8}"
            f"{state.population('vampires'):>7}  {symmetry}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
