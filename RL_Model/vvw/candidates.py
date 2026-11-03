"""Proposing the legal turns a policy chooses between.

This lived inside `VvWEnv` until self-play needed it. The environment builds
candidates for the side to move; a network playing the *other* side needs the
identical pipeline, or the two sides would be choosing from different action
spaces and the self-play game would not be a fair mirror.

Nothing here imports torch, so `vvw.agents` and `main.py` stay free of it.
"""

from __future__ import annotations

import random
from typing import List, Optional, Tuple

import numpy as np

from .agents import heuristic, naive, random_legal
from .evaluate import DEFAULT_WEIGHTS, evaluate
from .features import (
    CANDIDATE_FEATURES,
    candidate_features,
    encode_planes,
    encode_scalars,
)
from .rules import DEFAULT_UNKNOWNS, Move, Unknowns, validate_turn
from .simulator import apply_turn
from .state import GameState

#: Upper bound on candidate turns offered per decision.
MAX_CANDIDATES = 16


def raw_candidates(
    state: GameState, me: str, rng: Optional[random.Random] = None
) -> List[List[Move]]:
    """Where the proposals come from.

    The heuristic's own pick, plus wider beams for variety, plus the simpler
    agents so the policy can still reach turns the heuristic would never
    suggest.
    """
    # The mix matters. With three structured proposals against sixteen random
    # ones, strategically interesting turns were 1% of the pool even though
    # the heuristic itself splits on 24% of its turns -- the random entries
    # simply crowded them out, and a policy cannot learn to prefer a kind of
    # turn it is almost never offered.
    #
    # Several beam widths give genuinely different plans, because the beam
    # decides how many group-by-group alternatives survive assembly, so a
    # narrow beam commits early and a wide one finds coordinated turns a
    # narrow one prunes away.
    rng = rng or random.Random()
    proposals = [
        heuristic.choose_turn(state, me, beam=width) for width in (4, 8, 16, 24)
    ]
    proposals.append(naive.choose_turn(state, me))
    # Some random turns stay: they are the exploration end of the action
    # space, and without them the policy can only ever rerank the heuristic's
    # own ideas.
    for _ in range(MAX_CANDIDATES // 2):
        proposals.append(random_legal.choose_turn(state, me, rng))
    return proposals


def propose(
    state: GameState,
    me: str,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
    rng: Optional[random.Random] = None,
) -> Tuple[List[List[Move]], np.ndarray, int]:
    """Legal candidate turns, deduplicated, in a **shuffled** order.

    Returns the turns, their features, and where the heuristic's own top pick
    ended up.

    The shuffle matters. Handing them over sorted by heuristic score makes
    "copy the heuristic" mean "always output index 0", which a network can
    satisfy from the position alone without ever reading a candidate's
    features -- and a policy that has learnt a position cannot transfer that
    to a board where the ordering differs, nor ever discover that candidate 5
    was the better move. Shuffling forces every decision to come from the
    features, and leaves the heuristic's index as an honest supervision
    target for the warm start.
    """
    rng = rng or random.Random()
    state.me = me
    empty = np.zeros((MAX_CANDIDATES, CANDIDATE_FEATURES), dtype=np.float32)
    if not state.groups(me):
        return [], empty, 0

    scored = []
    seen = set()
    for turn in raw_candidates(state, me, rng):
        if not turn or validate_turn(state, turn, me, unknowns):
            continue
        signature = tuple(sorted(tuple(m) for m in turn))
        if signature in seen:
            continue
        seen.add(signature)
        after = apply_turn(
            state, turn, me, unknowns=unknowns, validate=False, expected=True
        )
        score = evaluate(after, me, DEFAULT_WEIGHTS, unknowns)
        scored.append((score, turn, after))

    scored.sort(key=lambda triple: -triple[0])
    kept = scored[:MAX_CANDIDATES]
    if not kept:
        return [], empty, 0

    order = list(range(len(kept)))
    rng.shuffle(order)
    # Index 0 of `kept` is the heuristic's pick; find where it landed.
    heuristic_choice = order.index(0)

    features = empty.copy()
    turns = []
    for slot, source in enumerate(order):
        score, turn, after = kept[source]
        features[slot] = candidate_features(
            state,
            after,
            turn,
            me,
            score,
            # `source` is the position in the heuristic's own ranking, so this
            # is distinct for every candidate and no two can present the
            # network with the same vector.
            rank=source / max(1, len(kept) - 1),
        )
        turns.append(turn)
    return turns, features, heuristic_choice


def observation_for(
    state: GameState,
    me: str,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
    rng: Optional[random.Random] = None,
):
    """What one side sees, and the turns it may pick from.

    The learner gets this through `VvWEnv._observe`; a network playing the
    other side gets it through here, so both read the same thing. Keeping it
    in this module rather than in `env` matters on tournament day: `env`
    imports gymnasium, and the client has no reason to need it.
    """
    turns, features, _ = propose(state, me, unknowns, rng)
    mask = np.zeros(MAX_CANDIDATES, dtype=np.int8)
    mask[: len(turns)] = 1
    return {
        "board": encode_planes(state, me),
        "scalars": encode_scalars(state, me, unknowns),
        "candidates": features,
        "mask": mask,
    }, turns
