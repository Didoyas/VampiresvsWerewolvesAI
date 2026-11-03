"""Measure a trained checkpoint against the agents that already play.

    python tools/evaluate_model.py --checkpoint runs/train1/best.pt

Evaluation uses the **test** seed range. Training's own periodic evaluation
uses validation, so reporting validation numbers as the result would be
reporting the number that was optimised against.

Every pairing is played on the same maps as both species and in both move
orders, because the opening side has a real advantage and a one-sided sample
measures that instead of the agent. Equal creature counts at the cap are a
genuine draw and are reported as such.

An untrained network is included on purpose. It is the floor that says how
much of any result came from training rather than from the candidate
generator doing the work underneath.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import torch  # noqa: E402

from config import PRESETS  # noqa: E402
from vvw import mapgen  # noqa: E402
from vvw.agents import heuristic as heuristic_agent  # noqa: E402
from vvw.agents import naive as naive_agent  # noqa: E402
from vvw.env import VvWEnv  # noqa: E402
from vvw.network import PolicyValueNet  # noqa: E402
from vvw.state import VAMPIRES, WEREWOLVES, opponent_of  # noqa: E402

OPPONENTS = {
    "heuristic": heuristic_agent.choose_turn,
    "naive": naive_agent.choose_turn,
}


def load_network(path: Optional[str], cfg) -> PolicyValueNet:
    network = PolicyValueNet(
        channels=cfg.network.channels,
        blocks=cfg.network.blocks,
        kernel=cfg.network.kernel,
        head_hidden=cfg.network.head_hidden,
    )
    if path:
        checkpoint = torch.load(path, map_location="cpu")
        network.load_state_dict(checkpoint["network"])
    return network.eval()


def _stack(observation) -> Dict[str, torch.Tensor]:
    import numpy as np

    return {
        key: torch.as_tensor(np.stack([observation[key]])).to(torch.float32)
        for key in ("board", "scalars", "candidates", "mask")
    }


@torch.no_grad()
def play_series(
    policy: Optional[PolicyValueNet],
    opponent: str,
    maps: int,
    split: str = "test",
    sizes: Optional[Tuple[int, int]] = None,
) -> Dict[str, float]:
    """`policy=None` plays the heuristic's own pick, as a reference line.

    `sizes` restricts the board to a (smallest, largest) side. The network is
    size-independent by construction -- fully convolutional, then globally
    pooled -- but that only means one set of weights *accepts* any board, not
    that it plays all of them equally well. The FCN paper this design borrows
    from reports exactly that gap: 94.7% on the board it trained on, 64-78%
    on larger unseen ones. Asking per size is the only way to know.
    """
    env = VvWEnv(split=split, opponents={opponent: OPPONENTS[opponent]}, shaping=0.0)
    if sizes is not None:
        env.board_range = sizes
    wins = losses = draws = 0
    margin = 0.0

    for index, seed in enumerate(mapgen.seeds(split, maps)):
        for species in (VAMPIRES, WEREWOLVES):
            for first in (True, False):
                observation, info = env.reset(
                    seed=seed,
                    options={
                        "map_seed": seed,
                        "species": species,
                        "opponent": opponent,
                        "we_move_first": first,
                    },
                )
                for _ in range(400):
                    if policy is None:
                        action = info["heuristic_choice"]
                    else:
                        action = int(
                            policy.act(_stack(observation), deterministic=True)[0][0]
                        )
                    observation, reward, terminated, truncated, info = env.step(action)
                    if terminated or truncated:
                        wins += reward > 0
                        losses += reward < 0
                        draws += reward == 0
                        break
                margin += env.state.population(env.me) - env.state.population(
                    opponent_of(env.me)
                )

    played = max(1, wins + losses + draws)
    return {
        "games": played,
        "wins": wins,
        "losses": losses,
        "draws": draws,
        "win_rate": round(wins / played, 4),
        "mean_margin": round(margin / played, 2),
    }


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", default=None)
    parser.add_argument("--preset", default="default", choices=sorted(PRESETS))
    parser.add_argument("--maps", type=int, default=15)
    parser.add_argument("--split", default="test", choices=sorted(mapgen.SPLITS))
    parser.add_argument(
        "--by-size",
        action="store_true",
        help="break the result down by board size, to see whether one set of "
        "weights really does play every size equally well",
    )
    args = parser.parse_args(argv)

    cfg = PRESETS[args.preset]

    if args.by_size:
        return _report_by_size(args, cfg)

    rows = []
    for opponent in ("heuristic", "naive"):
        rows.append(
            ("heuristic's own pick", opponent, play_series(None, opponent, args.maps, args.split))
        )
        rows.append(
            ("untrained network", opponent,
             play_series(load_network(None, cfg), opponent, args.maps, args.split))
        )
        if args.checkpoint:
            rows.append(
                (f"trained ({Path(args.checkpoint).name})", opponent,
                 play_series(load_network(args.checkpoint, cfg), opponent,
                             args.maps, args.split))
            )

    print(f"{args.split} split, {args.maps} maps, paired both species and both orders\n")
    print(f"{'policy':28s} {'opponent':10s} {'W':>4} {'L':>4} {'D':>4} {'win':>7} {'margin':>8}")
    for label, opponent, r in rows:
        print(f"{label:28s} {opponent:10s} {r['wins']:>4} {r['losses']:>4} "
              f"{r['draws']:>4} {r['win_rate']:>6.1%} {r['mean_margin']:>+8.1f}")
    print("\n" + json.dumps([{"policy": a, "opponent": b, **c} for a, b, c in rows]))
    return 0


def _report_by_size(args, cfg) -> int:
    """Where does it generalise, and where does it fall over?"""
    bands = [(10, 15), (16, 25), (26, 38), (39, 50)]
    policies = [("heuristic's own pick", None)]
    if args.checkpoint:
        policies.append((f"trained ({Path(args.checkpoint).name})", args.checkpoint))
    policies.append(("untrained network", "__untrained__"))

    print(
        f"{args.split} split, {args.maps} maps per band, "
        "paired both species and both move orders, opponent: heuristic"
    )
    print()
    header = f"{'policy':28s}" + "".join(f"{f'{a}-{b}':>14s}" for a, b in bands)
    print(header)
    print(f"{'':28s}" + "".join(f"{'win / margin':>14s}" for _ in bands))

    for label, source in policies:
        network = None
        if source == "__untrained__":
            network = load_network(None, cfg)
        elif source:
            network = load_network(source, cfg)

        cells = []
        for band in bands:
            result = play_series(
                network, "heuristic", args.maps, args.split, sizes=band
            )
            cells.append(f"{result['win_rate']:.0%} /{result['mean_margin']:+6.1f}")
        print(f"{label:28s}" + "".join(f"{c:>14s}" for c in cells))

    print()
    print(
        "A policy that holds up across the row generalises. One that is fine "
        "on 10-15 and poor on 39-50 has learnt the boards it was trained on, "
        "and the curriculum in config.py is where to look."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
