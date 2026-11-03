"""The opponent-pool agents and the policy/value network."""

import random

import numpy as np
import pytest

from vvw import mapgen
from vvw.agents import aggressive, heuristic, naive, random_legal
from vvw.features import CANDIDATE_FEATURES, PLANES, SCALARS
from vvw.rules import validate_turn
from vvw.simulator import apply_turn
from vvw.state import GameState, VAMPIRES, WEREWOLVES

torch = pytest.importorskip("torch")

POOL = {
    "random": random_legal.choose_turn,
    "naive": naive.choose_turn,
    "aggressive": aggressive.choose_turn,
    "heuristic": heuristic.choose_turn,
}


# -------------------------------------------------------------------- agents


@pytest.mark.parametrize("name", sorted(POOL))
def test_every_agent_plays_only_legal_turns(name):
    agent = POOL[name]
    random_legal.seed(1)
    for seed in range(4):
        state = mapgen.generate(seed)
        rng = random.Random(seed)
        for ply in range(30):
            species = VAMPIRES if ply % 2 == 0 else WEREWOLVES
            state.me = species
            turn = agent(state, species)
            assert turn, f"{name}: rule 1 requires a move"
            assert validate_turn(state, turn, species) == [], name
            state = apply_turn(state, turn, species, rng=rng, validate=False)


def test_the_random_agent_is_reproducible_from_a_seed():
    state = mapgen.generate(5)
    random_legal.seed(99)
    first = random_legal.choose_turn(state, VAMPIRES)
    random_legal.seed(99)
    assert random_legal.choose_turn(state, VAMPIRES) == first


def test_the_random_agent_does_not_always_do_the_same_thing():
    state = mapgen.generate(5)
    random_legal.seed(3)
    turns = {tuple(tuple(m) for m in random_legal.choose_turn(state, VAMPIRES)) for _ in range(30)}
    assert len(turns) > 3


def test_the_aggressive_agent_attacks_when_it_can_win():
    state = GameState(rows=10, columns=10, me=VAMPIRES)
    state.set_cell((4, 4), (0, 9, 0))
    state.set_cell((4, 5), (0, 0, 6))  # 2*9 >= 3*6, a certain kill
    turn = aggressive.choose_turn(state, VAMPIRES)
    assert any(move.target == (4, 5) for move in turn)


def test_the_aggressive_agent_still_refuses_a_losing_fight():
    state = GameState(rows=10, columns=10, me=VAMPIRES)
    state.set_cell((4, 4), (0, 4, 0))
    state.set_cell((4, 5), (0, 0, 30))
    turn = aggressive.choose_turn(state, VAMPIRES)
    assert all(move.target != (4, 5) for move in turn)


def test_combat_needs_an_asymmetric_start_to_happen_at_all():
    """Even symmetric starts stay peaceful, which is why `--handicap` exists.

    Two sides on a mirrored map grow in lockstep and neither reaches the 1.5x
    bar, so a whole game passes without a fight -- even when one of them is
    the agent written to pick fights. Splitting briefly broke this, because
    unguarded halves were small enough to be worth attacking; with
    `_would_be_overpowered` and two-step threat detection in place the agent
    declines those exposures again and the count is back to zero.

    The consequence for measurement: a benchmark of even symmetric maps never
    exercises the combat rules at all.
    """
    def fights(handicap: float) -> int:
        total = 0
        for seed in range(6):
            state = mapgen.generate(seed)
            if handicap != 1.0:
                for coord, count in state.groups(VAMPIRES):
                    state.set_cell(coord, (0, int(count * handicap), 0))
            rng = random.Random(seed)
            for ply in range(60):
                species = VAMPIRES if ply % 2 == 0 else WEREWOLVES
                enemy = WEREWOLVES if species == VAMPIRES else VAMPIRES
                agent = (
                    aggressive.choose_turn
                    if species == VAMPIRES
                    else heuristic.choose_turn
                )
                state.me = species
                before = state.population(enemy)
                turn = agent(state, species)
                state = apply_turn(state, turn, species, rng=rng, validate=False)
                if state.population(enemy) < before:
                    total += 1
        return total

    assert fights(1.0) == 0, "an even symmetric start should stay peaceful"
    assert fights(3.0) > 0, "a head start should produce fights"


# ------------------------------------------------------------- splitting


def test_it_splits_to_take_two_affordable_houses_at_once():
    """Harvesting is parallel and the clock is short: about 100 turns a side.

    Before this intent existed the agent split on 1.0% of its turns against
    the random agent's 67%, so every split the policy ever saw was a random
    one, and a strategy it is never shown cannot be learnt.
    """
    state = GameState(rows=20, columns=20, me=VAMPIRES)
    state.set_cell((10, 10), (0, 20, 0))
    state.set_cell((4, 10), (8, 0, 0))
    state.set_cell((16, 10), (8, 0, 0))

    turn = heuristic.choose_turn(state, VAMPIRES)
    sources = [move.source for move in turn]
    assert sources.count((10, 10)) >= 2, (
        f"expected one cell to send two ways, got {[tuple(m) for m in turn]}"
    )
    assert sum(m.count for m in turn if m.source == (10, 10)) <= 20


def test_it_does_not_split_into_a_half_the_enemy_could_wipe_out():
    """Unguarded splitting turned 2 losses in 80 games into 5."""
    state = GameState(rows=20, columns=20, me=VAMPIRES)
    state.set_cell((10, 10), (0, 20, 0))
    state.set_cell((4, 10), (8, 0, 0))
    state.set_cell((16, 10), (8, 0, 0))
    # Strong enough to beat either half outright, and close enough to do it.
    state.set_cell((11, 11), (0, 0, 60))

    turn = heuristic.choose_turn(state, VAMPIRES)
    from collections import Counter

    fan_out = Counter(move.source for move in turn)
    assert fan_out[(10, 10)] < 2, (
        "it should keep the stack together next to a group that outnumbers it"
    )


def test_it_does_not_split_when_only_one_house_is_affordable():
    state = GameState(rows=20, columns=20, me=VAMPIRES)
    state.set_cell((10, 10), (0, 10, 0))
    state.set_cell((4, 10), (8, 0, 0))
    state.set_cell((16, 10), (40, 0, 0))  # far too big for the remainder

    turn = heuristic.choose_turn(state, VAMPIRES)
    from collections import Counter

    assert Counter(move.source for move in turn)[(10, 10)] < 2


# ------------------------------------------------------------------- network


def _batch(size=3, side=20):
    return {
        "board": torch.rand(size, PLANES, side, side),
        "scalars": torch.rand(size, SCALARS),
        "candidates": torch.rand(size, 16, CANDIDATE_FEATURES),
        "mask": torch.ones(size, 16),
    }


def _net():
    from vvw.network import PolicyValueNet

    return PolicyValueNet(channels=8, blocks=2, head_hidden=32)


def test_the_network_produces_one_logit_per_candidate_and_one_value():
    logits, values = _net()(_batch())
    assert logits.shape == (3, 16)
    assert values.shape == (3,)


@pytest.mark.parametrize("side", [10, 23, 50])
def test_one_network_handles_every_board_size(side):
    """Fully convolutional, so 10x10 and 50x50 share the same weights."""
    logits, values = _net()(_batch(side=side))
    assert logits.shape == (3, 16) and values.shape == (3,)


def test_masked_candidates_can_never_be_chosen():
    network = _net()
    batch = _batch(size=1)
    batch["mask"] = torch.zeros(1, 16)
    batch["mask"][0, :3] = 1
    with torch.no_grad():
        probabilities = torch.softmax(network(batch)[0], dim=1)
    assert float(probabilities[0, 3:].sum()) < 1e-6
    assert float(probabilities[0, :3].sum()) == pytest.approx(1.0, abs=1e-5)


def test_different_candidates_give_different_logits():
    """Otherwise the policy can only learn how far to trust the ranking."""
    network = _net()
    batch = _batch(size=1)
    batch["candidates"][0, 0] = 1.0
    batch["candidates"][0, 1] = -1.0
    logits, _ = network(batch)
    assert abs(float(logits[0, 0] - logits[0, 1])) > 1e-5


def test_the_network_is_trainable_end_to_end():
    from vvw.network import evaluate_actions

    network = _net()
    optimiser = torch.optim.Adam(network.parameters(), lr=1e-2)
    batch = _batch(size=8)
    actions = torch.zeros(8, dtype=torch.long)

    before = float(evaluate_actions(network, batch, actions)[0].mean())
    for _ in range(20):
        log_probs, _, _ = evaluate_actions(network, batch, actions)
        loss = -log_probs.mean()
        optimiser.zero_grad()
        loss.backward()
        optimiser.step()
    after = float(evaluate_actions(network, batch, actions)[0].mean())
    assert after > before, "the policy did not move towards the rewarded action"


# ------------------------------------------------------------------- shaping


def test_shaping_pays_for_converting_humans():
    """Rewarding only the lead gives nothing: symmetric maps keep it at zero."""
    from vvw.env import VvWEnv

    env = VvWEnv(shaping=1.0)
    _, info = env.reset(seed=2, options={"opponent": "naive"})
    rewarded = 0
    for _ in range(400):
        # Candidates are shuffled, so index 0 is an arbitrary turn; follow the
        # heuristic's own pick to measure a competently played episode.
        _, reward, terminated, truncated, info = env.step(info["heuristic_choice"])
        if terminated or truncated:
            break
        if reward > 0:
            rewarded += 1
    assert rewarded > 1, "shaping produced almost no signal"



# ------------------------------------------------------- gambling when behind


def test_the_battle_threshold_follows_the_score():
    """A fixed floor is wrong at both ends of the score."""
    from vvw.agents.heuristic import battle_threshold

    def position(mine, theirs, humans=0, plies=0):
        state = GameState(rows=10, columns=10, me=VAMPIRES)
        state.set_cell((0, 0), (0, mine, 0))
        state.set_cell((9, 9), (0, 0, theirs))
        if humans:
            state.set_cell((5, 5), (humans, 0, 0))
        state.plies = {VAMPIRES: plies, WEREWOLVES: plies}
        return state

    ahead = battle_threshold(position(20, 10), VAMPIRES)
    level = battle_threshold(position(15, 15), VAMPIRES)
    behind = battle_threshold(position(12, 15), VAMPIRES)
    desperate = battle_threshold(position(12, 15, plies=95), VAMPIRES)

    assert ahead > level > behind > desperate
    assert ahead >= 0.8, "with a won game, do not gamble it"
    assert desperate <= 0.25, "with a lost game, take the only chance there is"


def test_being_behind_with_nothing_to_harvest_lowers_the_bar():
    """Harvesting can still save a game while a house is affordable."""
    from vvw.agents.heuristic import battle_threshold

    def position(humans):
        state = GameState(rows=10, columns=10, me=VAMPIRES)
        state.set_cell((0, 0), (0, 12, 0))
        state.set_cell((9, 9), (0, 0, 15))
        if humans:
            state.set_cell((5, 5), (humans, 0, 0))
        return state

    assert battle_threshold(position(5), VAMPIRES) > battle_threshold(
        position(0), VAMPIRES
    )


def test_a_losing_deadlock_is_broken_rather_than_sat_out():
    """12 against 15 with nothing left to take.

    Sitting still is a certain loss, because equal counts would be a draw and
    we are not even level. Attacking is a 40% chance to wipe them out and win
    outright. The agent used to decline it and play out the clock.
    """
    state = GameState(rows=6, columns=6, me=VAMPIRES)
    state.set_cell((2, 2), (0, 12, 0))
    state.set_cell((3, 3), (0, 0, 15))
    state.plies = {VAMPIRES: 90, WEREWOLVES: 90}

    turn = heuristic.choose_turn(state, VAMPIRES)
    assert any(move.target == (3, 3) for move in turn), (
        "with the game otherwise lost, the agent should take the fight"
    )


def test_a_won_game_is_not_gambled_away():
    state = GameState(rows=6, columns=6, me=VAMPIRES)
    state.set_cell((2, 2), (0, 20, 0))
    state.set_cell((3, 3), (0, 0, 16))  # 2*20 < 3*16, so only a random battle
    state.plies = {VAMPIRES: 90, WEREWOLVES: 90}

    turn = heuristic.choose_turn(state, VAMPIRES)
    assert all(move.target != (3, 3) for move in turn), (
        "already ahead, so a coin flip can only lose the game"
    )


# ------------------------------------------------------------------ pooling


@pytest.mark.parametrize("max_pool", [False, True])
def test_both_pooling_modes_produce_the_same_shapes(max_pool):
    from vvw.network import PolicyValueNet

    network = PolicyValueNet(
        channels=8, blocks=2, head_hidden=32, max_pool=max_pool
    )
    logits, values = network(_batch())
    assert logits.shape == (3, 16) and values.shape == (3,)


def test_max_pooling_sees_concentration_that_averaging_misses():
    """One large stack and the same creatures spread thin have equal means.

    The 1.5x threshold is about concentration, so a representation that
    cannot tell those apart is missing something the rules care about.
    """
    from vvw.features import encode_planes
    from vvw.network import PolicyValueNet

    concentrated = GameState(rows=10, columns=10, me=VAMPIRES)
    concentrated.set_cell((5, 5), (0, 60, 0))
    concentrated.set_cell((0, 0), (0, 0, 10))

    spread = GameState(rows=10, columns=10, me=VAMPIRES)
    for index in range(6):
        spread.set_cell((index, 9), (0, 10, 0))
    spread.set_cell((0, 0), (0, 0, 10))

    boards = torch.as_tensor(
        np.stack([encode_planes(concentrated), encode_planes(spread)])
    )

    def spread_of_pooled(max_pool):
        torch.manual_seed(0)  # same weights both ways, or this measures noise
        network = PolicyValueNet(
            channels=8, blocks=2, head_hidden=32, max_pool=max_pool
        ).eval()
        with torch.no_grad():
            spatial = network.trunk(network.stem(boards))
            pooled = network.pool(spatial).flatten(1)
            if network.peak is not None:
                pooled = torch.cat(
                    [pooled, network.peak(spatial).flatten(1)], dim=1
                )
        return float((pooled[0] - pooled[1]).abs().max())

    assert spread_of_pooled(True) > spread_of_pooled(False)


def test_the_network_is_already_invariant_to_reorientation():
    """Why the paper's symmetry augmentation is a no-op here.

    A global pool over spatial positions is invariant to any permutation of
    them, so rotating or reflecting the board cannot change the logits. The
    game's symmetry comes for free from the architecture, rather than having
    to be bought with four to eight times the training data.
    """
    from vvw.augment import augment_planes
    from vvw.features import encode_planes
    from vvw.network import PolicyValueNet

    state = mapgen.generate(11, rows=16, columns=16)
    original = encode_planes(state)
    network = PolicyValueNet(channels=8, blocks=2, head_hidden=32).eval()

    # One fixed batch, with only the board swapped: the scalars and candidate
    # features must stay identical or the comparison measures those instead.
    fixed = _batch(size=1)

    def logits_for(planes):
        batch = {**fixed, "board": torch.as_tensor(planes[None])}
        with torch.no_grad():
            return network(batch)[0]

    # Not exactly zero: padded convolutions produce slightly different edge
    # activations when the board is reoriented inside its padding. Measured at
    # about 1e-4, against about 2e-2 for a trunk that is genuinely not
    # invariant, so the two cases are three orders of magnitude apart.
    base = logits_for(original)
    for name in ("rotate180", "flip_vertical", "flip_horizontal"):
        reoriented = augment_planes(original, name)
        assert not np.allclose(reoriented, original), f"{name} changed nothing"
        difference = float((base - logits_for(reoriented)).abs().max())
        assert difference < 1e-3, (
            f"{name} moved the logits by {difference:.1e}, so the trunk is not "
            "invariant after all and symmetry augmentation would be worth having"
        )


def test_max_pooling_is_what_would_break_that_invariance():
    """Documents why `max_pool` defaults to off.

    Max takes the single largest activation, and that can be a padding-edge
    artefact, so unlike averaging it notices where the board sits inside its
    padding. It bought no measurable accuracy and cost this property.
    """
    from vvw.augment import augment_planes
    from vvw.features import encode_planes
    from vvw.network import PolicyValueNet

    original = encode_planes(mapgen.generate(11, rows=16, columns=16))
    reflected = augment_planes(original, "flip_horizontal")
    fixed = _batch(size=1)

    def drift(max_pool):
        torch.manual_seed(0)  # same weights both ways, or this measures noise
        network = PolicyValueNet(
            channels=8, blocks=2, head_hidden=32, max_pool=max_pool
        ).eval()
        with torch.no_grad():
            before = network({**fixed, "board": torch.as_tensor(original[None])})[0]
            after = network({**fixed, "board": torch.as_tensor(reflected[None])})[0]
        return float((before - after).abs().max())

    assert drift(True) > 20 * drift(False)
