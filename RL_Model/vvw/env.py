"""A Gymnasium environment around our own simulator.

One `step` is one complete turn of ours followed by the opponent's reply, so
the agent sees the board only when it is its move.

**The action space is a choice among generated candidate turns, not a raw
move.** A turn in this game is a compound action: any subset of our groups,
each moving wholly or in part, in any of eight directions. Emitting that
directly needs an autoregressive policy over (source, destination, count,
continue) with legality masks at every step, and the reward — who was ahead
when the clock ran out — has to travel back through all of it. Here the
heuristic's intent generator proposes a handful of legal, tactically
meaningful turns and the policy picks one. The combinatorics stay in code
that is already tested, and learning reduces to a small discrete choice.

The cost is real and worth stating: the agent can never play a turn the
generator did not propose, so its ceiling is the generator's coverage. That
is the trade, and `info["candidates"]` is there to measure it.

Gymnasium is only the environment interface. No part of the game, the search
or the learning comes from a library.
"""

from __future__ import annotations

import random
from typing import Any, Dict, List, Optional, Tuple

import gymnasium as gym
import numpy as np
from gymnasium import spaces

from . import mapgen
from .candidates import (
    MAX_CANDIDATES,
    observation_for,
    propose,
    raw_candidates,
)
from .agents import heuristic, naive, random_legal
from .evaluate import DEFAULT_WEIGHTS, evaluate
from .features import (
    CANDIDATE_FEATURES,
    PLANES,
    SCALARS,
    candidate_features,
    encode_planes,
    encode_scalars,
)
from .mapgen import MAX_SIDE
from .rules import DEFAULT_UNKNOWNS, Move, Unknowns, validate_turn
from .simulator import apply_turn, is_terminal, winner
from .state import GameState, VAMPIRES, WEREWOLVES, opponent_of

#: Upper bound on candidate turns offered per decision. Re-exported from
#: `candidates`, which is where proposing actually lives, so that the
#: tournament client can reach it without importing gymnasium.
MAX_CANDIDATES = MAX_CANDIDATES

#: Opponents to train against. A pool, not a single sparring partner: an
#: agent trained only against one opponent learns that opponent.
OPPONENT_POOL = {
    "random": random_legal.choose_turn,
    "naive": naive.choose_turn,
    "heuristic": heuristic.choose_turn,
}

#: The self-play opponent: a past version of the learner itself. It is only
#: offered in the draw once `snapshots` holds something, so a run configured
#: for self-play still opens against the scripted pool and never faces an
#: untrained network -- which would teach it nothing and reward nonsense.
SELFPLAY = "selfplay"


class VvWEnv(gym.Env):
    """Vampires VS Werewolves, from one side's point of view."""

    metadata = {"render_modes": []}

    def __init__(
        self,
        split: str = "train",
        opponents: Optional[Dict[str, Any]] = None,
        shaping: float = 0.0,
        unknowns: Unknowns = DEFAULT_UNKNOWNS,
        opponent_mix: Optional[Dict[str, float]] = None,
    ):
        super().__init__()
        self.split = split
        self.opponents = dict(opponents or OPPONENT_POOL)
        self.shaping = shaping
        self.unknowns = unknowns
        # How often each opponent is drawn. `config.opponent_mix` existed for
        # a while and was never read: every opponent was picked with equal
        # probability no matter what the config said, so the random agent got
        # a third of all training games instead of the 15% intended.
        self.opponent_mix = dict(opponent_mix) if opponent_mix else None

        #: Past versions of the learner, as CPU state dicts, newest last.
        #: `train.py` pushes these into every worker with `set_attr`.
        self._snapshots: List[Any] = []
        self._snapshot_agents: Dict[int, Any] = {}
        self._snapshot_index = 0
        #: The shape the snapshots were trained at, needed to rebuild them:
        #: {"channels": ..., "blocks": ..., "head_hidden": ...}.
        self.network_config: Optional[Dict[str, int]] = None

        self.observation_space = spaces.Dict(
            {
                "board": spaces.Box(
                    0.0, 1.0, shape=(PLANES, MAX_SIDE, MAX_SIDE), dtype=np.float32
                ),
                "scalars": spaces.Box(-1.0, 1.0, shape=(SCALARS,), dtype=np.float32),
                "candidates": spaces.Box(
                    -1.0,
                    1.0,
                    shape=(MAX_CANDIDATES, CANDIDATE_FEATURES),
                    dtype=np.float32,
                ),
                "mask": spaces.MultiBinary(MAX_CANDIDATES),
            }
        )
        self.action_space = spaces.Discrete(MAX_CANDIDATES)

        self.state: GameState = GameState()
        self.me: str = VAMPIRES
        self._rng = random.Random()
        self._opponent_name = "naive"
        #: (smallest, largest) board side to generate, or None for the map
        #: generator's own range. `train.py` moves this as the curriculum
        #: advances; before it was read here the curriculum did nothing at
        #: all and every run trained on the full 10-50 spread from step one.
        self.board_range: Optional[Tuple[int, int]] = None
        self._candidates: List[List[Move]] = []
        self._candidate_features = np.zeros(
            (MAX_CANDIDATES, CANDIDATE_FEATURES), dtype=np.float32
        )
        #: Where the heuristic's own pick sits in the shuffled candidates.
        self.heuristic_choice = 0

    # ---- self-play pool --------------------------------------------------

    @property
    def snapshots(self) -> List[Any]:
        return self._snapshots

    @snapshots.setter
    def snapshots(self, value: Optional[List[Any]]) -> None:
        # Rebuilding the agents is the entire cost of a new snapshot, so it
        # is paid here, once, rather than on every turn of every game.
        self._snapshots = list(value or [])
        self._snapshot_agents = {}

    def _available_opponents(self) -> List[str]:
        names = sorted(self.opponents)
        if self._snapshots:
            names.append(SELFPLAY)
        return names

    def _agent_for(self, name: str) -> Any:
        if name != SELFPLAY:
            return self.opponents[name]
        index = self._snapshot_index
        if index not in self._snapshot_agents:
            from .agents.policy import PolicyAgent

            self._snapshot_agents[index] = PolicyAgent.from_snapshot(
                self._snapshots[index],
                unknowns=self.unknowns,
                **(self.network_config or {}),
            )
        return self._snapshot_agents[index]

    # ---- gymnasium API ---------------------------------------------------

    def reset(self, *, seed: Optional[int] = None, options: Optional[Dict] = None):
        super().reset(seed=seed)
        options = options or {}
        self._rng = random.Random(seed)

        pool = mapgen.SPLITS[self.split]
        preset_board = options.get("board")
        if preset_board is not None:
            # A specific board, used to replay the maps that shipped with the
            # assignment rather than generated ones.
            map_seed = options.get("map_seed", 0)
            self.state = preset_board.copy()
            self.state.houses = set(preset_board.houses)
            self._finish_reset(options, map_seed)
            return self._after_reset(map_seed)

        map_seed = options.get("map_seed", self._rng.choice(pool))
        if self.board_range is None:
            self.state = mapgen.generate(map_seed)
        else:
            # Derive the size from the same seed, so a given seed still names
            # one reproducible map for a given curriculum stage.
            low, high = self.board_range
            sizer = random.Random(map_seed)
            self.state = mapgen.generate(
                map_seed,
                rows=sizer.randint(low, high),
                columns=sizer.randint(low, high),
            )
        self._finish_reset(options, map_seed)
        return self._after_reset(map_seed)

    def _finish_reset(self, options: Dict, map_seed: int) -> None:
        self.me = options.get("species", self._rng.choice([VAMPIRES, WEREWOLVES]))
        self._opponent_name = options.get("opponent") or self._draw_opponent()
        if self._opponent_name == SELFPLAY and not self._snapshots:
            # Asked for explicitly before any snapshot exists.
            self._opponent_name = (
                "heuristic" if "heuristic" in self.opponents else self._available_opponents()[0]
            )
        if self._opponent_name == SELFPLAY:
            # One sparring partner for the whole game. Drawing a fresh
            # snapshot each turn would give the learner an opponent whose
            # plan changes under it, which is not a player at all.
            self._snapshot_index = self._rng.randrange(len(self._snapshots))
        random_legal.seed(map_seed)

        # Half the time the opponent opens, so the agent learns both roles.
        self._we_move_first = options.get("we_move_first", self._rng.random() < 0.5)
        if not self._we_move_first:
            self._opponent_turn()

    def _after_reset(self, map_seed: int):
        observation = self._observe()
        return observation, {
            "map_seed": map_seed,
            "opponent": self._opponent_name,
            "heuristic_choice": self.heuristic_choice,
        }

    def step(self, action: int):
        if not self._candidates:
            # Nothing legal to do, which means we have no creatures left.
            return self._observe(), -1.0, True, False, {"reason": "no candidates"}

        before = self._potential()
        turn = self._candidates[min(int(action), len(self._candidates) - 1)]
        self.state = apply_turn(
            self.state,
            turn,
            self.me,
            rng=self._rng,
            unknowns=self.unknowns,
            validate=False,
        )

        terminated = is_terminal(self.state, self.unknowns)
        if not terminated:
            self._opponent_turn()
            terminated = is_terminal(self.state, self.unknowns)

        reward = 0.0
        if terminated:
            reward = self._terminal_reward()
        elif self.shaping:
            reward = self.shaping * (self._potential() - before)

        observation = self._observe()
        info = {
            "candidates": len(self._candidates),
            "opponent": self._opponent_name,
            "heuristic_choice": self.heuristic_choice,
        }
        return observation, reward, terminated, False, info

    # ---- internals -------------------------------------------------------

    def _draw_opponent(self) -> str:
        """Pick an opponent, honouring the configured mix when there is one."""
        names = self._available_opponents()
        if not self.opponent_mix:
            return self._rng.choice(names)
        weights = [max(0.0, self.opponent_mix.get(name, 0.0)) for name in names]
        if sum(weights) <= 0:
            return self._rng.choice(names)
        return self._rng.choices(names, weights=weights, k=1)[0]

    def _opponent_turn(self) -> None:
        them = opponent_of(self.me)
        if not self.state.groups(them):
            return
        agent = self._agent_for(self._opponent_name)
        turn = agent(self.state, them)
        if turn and not validate_turn(self.state, turn, them, self.unknowns):
            self.state = apply_turn(
                self.state,
                turn,
                them,
                rng=self._rng,
                unknowns=self.unknowns,
                validate=False,
            )
        else:
            # A pool agent produced nothing legal; charge it the turn anyway
            # so the move cap still advances and the game cannot hang.
            self.state.plies[them] = self.state.plies.get(them, 0) + 1

    def _terminal_reward(self) -> float:
        first_mover = self.me if self._we_move_first else opponent_of(self.me)
        champion = winner(self.state, first_mover, self.unknowns)
        if champion is None:
            return 0.0
        return 1.0 if champion == self.me else -1.0

    def _potential(self) -> float:
        """The quantity shaping rewards improving.

        Two terms, because one is not enough. The lead alone is useless as a
        signal here: generated maps are symmetric, so both sides harvest
        mirror halves, the lead stays at zero all game, and a measured
        episode produced exactly one non-zero reward in a hundred steps — the
        terminal one. Adding the share of the board we have taken makes
        converting a house pay immediately, which is what the early game is
        actually about.

        It is a potential, and shaping is its difference between states, so
        in the limit it does not change which policy is optimal; it only
        makes the gradient findable before then.
        """
        mine = self.state.population(self.me)
        theirs = self.state.population(opponent_of(self.me))
        humans = self.state.human_population()
        lead = (mine - theirs) / max(1, mine + theirs)
        share = mine / max(1, mine + theirs + humans)
        return lead + share

    def _population_gap(self) -> float:
        mine = self.state.population(self.me)
        theirs = self.state.population(opponent_of(self.me))
        return (mine - theirs) / max(1, mine + theirs)

    def _observe(self) -> Dict[str, np.ndarray]:
        self.state.me = self.me
        self._candidates, self._candidate_features, self.heuristic_choice = (
            self._propose()
        )
        mask = np.zeros(MAX_CANDIDATES, dtype=np.int8)
        mask[: len(self._candidates)] = 1
        return {
            "board": encode_planes(self.state, self.me),
            "scalars": encode_scalars(self.state, self.me, self.unknowns),
            "candidates": self._candidate_features,
            "mask": mask,
        }

    def _propose(self) -> Tuple[List[List[Move]], np.ndarray, int]:
        return propose(self.state, self.me, self.unknowns, self._rng)

    def _raw_candidates(self) -> List[List[Move]]:
        return raw_candidates(self.state, self.me, self._rng)
