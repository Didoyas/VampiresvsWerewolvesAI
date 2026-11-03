"""Play one game and record every board, so it can be watched afterwards.

    python tools/record_game.py --checkpoint best.pt --seed 200003 --out game.json

The output holds the whole game: the map, who played which side, and for every
ply the board before the move, the move itself, and which candidate the policy
picked out of how many it was offered. That last part is the interesting bit --
it shows not just what the agent did but what it was choosing between.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import numpy as np  # noqa: E402
import torch  # noqa: E402

from config import PRESETS  # noqa: E402
from vvw import mapgen  # noqa: E402
from vvw.agents import heuristic as heuristic_agent  # noqa: E402
from vvw.agents import naive as naive_agent  # noqa: E402
from vvw.env import VvWEnv  # noqa: E402
from vvw.network import PolicyValueNet  # noqa: E402
from vvw.state import GameState, VAMPIRES, WEREWOLVES, opponent_of  # noqa: E402

OPPONENTS = {"heuristic": heuristic_agent.choose_turn, "naive": naive_agent.choose_turn}


def snapshot(state: GameState) -> List[List[int]]:
    return [
        [x, y, h, v, w] for (x, y), (h, v, w) in sorted(state.cells.items())
    ]


def _stack(observation) -> Dict[str, torch.Tensor]:
    return {
        key: torch.as_tensor(np.stack([observation[key]])).to(torch.float32)
        for key in ("board", "scalars", "candidates", "mask")
    }


@torch.no_grad()
def record(
    checkpoint: Optional[str],
    seed: int,
    opponent: str,
    species: str,
    cfg,
    max_plies: int = 400,
    map_file: Optional[str] = None,
) -> Dict:
    network = None
    if checkpoint:
        network = PolicyValueNet(
            channels=cfg.network.channels,
            blocks=cfg.network.blocks,
            kernel=cfg.network.kernel,
            head_hidden=cfg.network.head_hidden,
        )
        network.load_state_dict(torch.load(checkpoint, map_location="cpu")["network"])
        network.eval()

    env = VvWEnv(
        split="test", opponents={opponent: OPPONENTS[opponent]}, shaping=0.0
    )
    options = {
        "map_seed": seed,
        "species": species,
        "opponent": opponent,
        "we_move_first": True,
    }
    if map_file:
        from vvw.maps import load_map

        options["board"] = load_map(map_file)
    observation, info = env.reset(seed=seed, options=options)

    plies = []
    for _ in range(max_plies):
        before = snapshot(env.state)
        offered = len(env._candidates)
        if network is None:
            action = info["heuristic_choice"]
            confidence = None
        else:
            logits, value = network(_stack(observation))
            probabilities = torch.softmax(logits[0, :offered], dim=0)
            action = int(probabilities.argmax())
            confidence = float(probabilities[action])
        chosen = [list(m) for m in env._candidates[action]] if offered else []
        agreed = action == info["heuristic_choice"]

        observation, reward, terminated, truncated, info = env.step(action)
        plies.append(
            {
                "board_before": before,
                "moves": chosen,
                "candidates_offered": offered,
                "confidence": None if confidence is None else round(confidence, 3),
                "agreed_with_heuristic": agreed,
                "us": env.state.population(env.me),
                "them": env.state.population(opponent_of(env.me)),
                "humans": env.state.human_population(),
            }
        )
        if terminated or truncated:
            break

    return {
        "rows": env.state.rows,
        "columns": env.state.columns,
        "seed": seed,
        "map_name": Path(map_file).stem if map_file else f"generated {seed}",
        "we_play": env.me,
        "opponent": opponent,
        "policy": Path(checkpoint).name if checkpoint else "heuristic",
        "final": {
            "us": env.state.population(env.me),
            "them": env.state.population(opponent_of(env.me)),
        },
        "plies": plies,
    }


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", default=None)
    parser.add_argument("--seed", type=int, default=200_003)
    parser.add_argument("--opponent", default="heuristic", choices=sorted(OPPONENTS))
    parser.add_argument("--species", default=VAMPIRES, choices=[VAMPIRES, WEREWOLVES])
    parser.add_argument("--preset", default="default", choices=sorted(PRESETS))
    parser.add_argument(
        "--map",
        default=None,
        help="an .xml map to play instead of a generated one",
    )
    parser.add_argument("--out", default="game.json")
    args = parser.parse_args(argv)

    game = record(
        args.checkpoint,
        args.seed,
        args.opponent,
        args.species,
        PRESETS[args.preset],
        map_file=args.map,
    )
    Path(args.out).write_text(json.dumps(game), encoding="utf-8")

    agreed = sum(1 for p in game["plies"] if p["agreed_with_heuristic"])
    print(f"{game['map_name']}: {game['rows']}x{game['columns']}")
    print(f"policy {game['policy']} as {game['we_play']} vs {game['opponent']}")
    print(f"{len(game['plies'])} plies -> {game['final']['us']} v {game['final']['them']}")
    print(f"agreed with the heuristic on {agreed}/{len(game['plies'])} decisions")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
