"""The training environment and its feature encoding."""

import numpy as np
import pytest

from config import PRESETS, board_range, shaping_at
from vvw import mapgen
from vvw.env import MAX_CANDIDATES, VvWEnv
from vvw.features import PLANES, SCALARS, encode_planes, encode_scalars
from vvw.mapgen import MAX_SIDE
from vvw.state import GameState, VAMPIRES, WEREWOLVES


# ------------------------------------------------------------------ features


def test_planes_have_the_declared_shape():
    planes = encode_planes(mapgen.generate(3))
    assert planes.shape == (PLANES, MAX_SIDE, MAX_SIDE)
    assert planes.dtype == np.float32


def test_every_plane_value_is_within_range():
    planes = encode_planes(mapgen.generate(11))
    assert planes.min() >= 0.0 and planes.max() <= 1.0


def test_padding_is_marked_so_a_small_board_is_not_a_deserted_big_one():
    state = mapgen.generate(4, rows=10, columns=12)
    planes = encode_planes(state)
    assert planes[4, :10, :12].all(), "the real board should be marked"
    assert not planes[4, 10:, :].any(), "padding should not be"
    assert not planes[4, :, 12:].any()


def test_the_board_is_seen_from_the_moving_side():
    """One network plays both species, so 'ours' is always plane 0."""
    state = GameState(rows=10, columns=10)
    state.set_cell((1, 1), (0, 7, 0))
    state.set_cell((8, 8), (0, 0, 3))
    ours = encode_planes(state, VAMPIRES)
    theirs = encode_planes(state, WEREWOLVES)
    assert ours[0, 1, 1] == theirs[1, 1, 1]
    assert ours[1, 8, 8] == theirs[0, 8, 8]


def test_coordinates_land_at_row_y_column_x():
    state = GameState(rows=10, columns=10)
    state.set_cell((3, 7), (0, 5, 0))  # x=3 is the column, y=7 the row
    planes = encode_planes(state, VAMPIRES)
    assert planes[0, 7, 3] > 0
    assert planes[0, 3, 7] == 0


def test_bigger_populations_encode_larger():
    small, large = GameState(rows=10, columns=10), GameState(rows=10, columns=10)
    small.set_cell((0, 0), (0, 3, 0))
    large.set_cell((0, 0), (0, 90, 0))
    assert encode_planes(large)[0, 0, 0] > encode_planes(small)[0, 0, 0]


def test_scalars_carry_who_is_ahead():
    state = GameState(rows=10, columns=10)
    state.set_cell((0, 0), (0, 20, 0))
    state.set_cell((9, 9), (0, 0, 5))
    ahead = encode_scalars(state, VAMPIRES)
    behind = encode_scalars(state, WEREWOLVES)
    assert ahead.shape == (SCALARS,)
    assert ahead[3] > 0 > behind[3]
    assert ahead[3] == pytest.approx(-behind[3])


# ----------------------------------------------------------------------- env


def test_reset_returns_an_observation_in_the_declared_space():
    env = VvWEnv()
    observation, info = env.reset(seed=1)
    assert env.observation_space.contains(observation)
    assert "map_seed" in info


def test_every_offered_candidate_is_a_legal_turn():
    from vvw.rules import validate_turn

    env = VvWEnv()
    env.reset(seed=2)
    for _ in range(12):
        for turn in env._candidates:
            assert validate_turn(env.state, turn, env.me) == []
        _, _, terminated, _, _ = env.step(0)
        if terminated:
            break


def test_the_mask_matches_the_number_of_candidates():
    env = VvWEnv()
    observation, _ = env.reset(seed=3)
    assert observation["mask"].sum() == len(env._candidates)
    assert 0 < len(env._candidates) <= MAX_CANDIDATES


def test_an_episode_terminates_within_the_move_cap():
    env = VvWEnv()
    env.reset(seed=4)
    for step in range(300):
        _, _, terminated, _, _ = env.step(0)
        if terminated:
            assert step < 250
            break
    else:
        pytest.fail("the episode never ended")


def test_the_terminal_reward_is_win_loss_or_draw():
    env = VvWEnv()
    for seed in range(4):
        env.reset(seed=seed)
        reward = 0.0
        for _ in range(300):
            _, reward, terminated, _, _ = env.step(0)
            if terminated:
                break
        assert reward in (-1.0, 0.0, 1.0)


def test_an_out_of_range_action_is_clamped_rather_than_crashing():
    env = VvWEnv()
    env.reset(seed=6)
    env.step(MAX_CANDIDATES * 5)  # must not raise


def test_shaping_rewards_gaining_ground():
    env = VvWEnv(shaping=1.0)
    env.reset(seed=7)
    rewards = [env.step(0)[1] for _ in range(20)]
    assert any(r != 0.0 for r in rewards)


def test_both_species_and_both_move_orders_occur():
    env = VvWEnv()
    seen = set()
    for seed in range(12):
        env.reset(seed=seed)
        seen.add((env.me, env._we_move_first))
    assert len(seen) >= 3


def test_the_opponent_pool_is_actually_varied():
    env = VvWEnv()
    used = {env.reset(seed=s)[1]["opponent"] for s in range(20)}
    assert len(used) > 1


# -------------------------------------------------------------------- config


def test_presets_exist_and_smoke_is_small():
    assert set(PRESETS) == {"default", "smoke", "selfplay"}
    assert PRESETS["smoke"].total_steps < PRESETS["default"].total_steps
    assert PRESETS["smoke"].network.channels < PRESETS["default"].network.channels


def test_the_curriculum_widens_as_training_goes_on():
    cfg = PRESETS["default"]
    early = board_range(cfg, 0)
    late = board_range(cfg, 2_000_000)
    assert late[1] > early[1]
    assert late[1] <= mapgen.MAX_SIDE


def test_shaping_decays_to_nothing():
    cfg = PRESETS["default"]
    assert shaping_at(cfg, 0) == cfg.shaping
    assert shaping_at(cfg, cfg.shaping_decay_steps) == 0.0
    assert shaping_at(cfg, cfg.shaping_decay_steps * 2) == 0.0


def test_the_opponent_mix_is_a_distribution():
    assert sum(PRESETS["default"].opponent_mix.values()) == pytest.approx(1.0)


# ------------------------------------------------------------- curriculum


def test_the_curriculum_actually_constrains_the_board():
    """`train.py` sets `board_range`; for a while nothing read it.

    The curriculum is meant to start on small boards, where games are short
    and the terminal reward arrives often, and widen as training goes on.
    Until the environment honoured the setting, every run trained on the full
    10-50 spread from the first step and the schedule was decoration.
    """
    env = VvWEnv()
    env.board_range = (10, 12)
    for seed in range(8):
        env.reset(seed=seed)
        assert 10 <= env.state.rows <= 12
        assert 10 <= env.state.columns <= 12


def test_clearing_the_curriculum_restores_the_full_range():
    env = VvWEnv()
    env.board_range = None
    sizes = set()
    for seed in range(12):
        env.reset(seed=seed)
        sizes.add((env.state.rows, env.state.columns))
    assert max(max(s) for s in sizes) > 12, "should reach beyond the small boards"


def test_a_seed_still_names_one_map_within_a_curriculum_stage():
    first, second = VvWEnv(), VvWEnv()
    first.board_range = second.board_range = (10, 20)
    first.reset(seed=3, options={"map_seed": 3})
    second.reset(seed=3, options={"map_seed": 3})
    assert first.state.cells == second.state.cells


# ------------------------------------------------- is the policy moving?


def test_greedy_churn_notices_a_policy_that_moved():
    """The health check that replaced entropy.

    Entropy was tried first and is blind to this: a configuration that
    changed 47% of its chosen moves in ten updates moved entropy by 0.03.
    Entropy describes the spread of the distribution, not whether the argmax
    moved, so churn is measured directly instead.
    """
    torch = pytest.importorskip("torch")
    from train import GreedyChurn
    from vvw.network import PolicyValueNet

    probe = GreedyChurn(size=12)
    if not probe.states:
        pytest.skip("probe set could not be built")

    network = PolicyValueNet(channels=8, blocks=2, head_hidden=32)
    assert probe.measure(network) is None, "the first call only sets a baseline"
    assert probe.measure(network) == 0.0, "an unchanged policy must show no churn"

    # Perturb the policy head hard enough that its preferences must change.
    with torch.no_grad():
        for parameter in network.policy.parameters():
            parameter.add_(torch.randn_like(parameter) * 2.0)
    assert probe.measure(network) > 0.0, "a moved policy must show churn"


# -------------------------------------------------- curriculum and opponents


def test_the_curriculum_reaches_the_full_range_on_a_short_run_too():
    """Thresholds are fractions of the run, not absolute step counts.

    They used to be absolute and sized for four million steps, so a 300k run
    never saw a board wider than 20 and a one-million-step run never passed
    35 -- while the tournament draws uniformly from 10x10 to 50x50.
    """
    from dataclasses import replace

    for total in (300_000, 1_000_000, 4_000_000):
        cfg = replace(PRESETS["default"], total_steps=total)
        assert board_range(cfg, 0)[1] < mapgen.MAX_SIDE, "it should start small"
        assert board_range(cfg, total - 1)[1] == mapgen.MAX_SIDE, (
            f"a {total}-step run must reach {mapgen.MAX_SIDE} before it ends"
        )
        # And it must get there with time left to learn on those boards.
        assert board_range(cfg, total // 2)[1] == mapgen.MAX_SIDE


def test_the_opponent_mix_is_actually_applied():
    """`config.opponent_mix` existed for a while and was never read.

    Every opponent was drawn with equal probability whatever the config said,
    so the random agent took a third of training games instead of the 15%
    intended.
    """
    from collections import Counter

    mix = {"random": 0.1, "naive": 0.1, "heuristic": 0.8}
    env = VvWEnv(opponent_mix=mix)
    seen = Counter(env.reset(seed=s)[1]["opponent"] for s in range(240))
    share = seen["heuristic"] / sum(seen.values())
    assert share > 0.6, f"heuristic should dominate the draw, got {share:.0%}"


def test_without_a_mix_the_draw_is_uniform():
    from collections import Counter

    env = VvWEnv()
    seen = Counter(env.reset(seed=s)[1]["opponent"] for s in range(240))
    shares = [v / sum(seen.values()) for v in seen.values()]
    assert max(shares) - min(shares) < 0.2, f"expected roughly uniform, got {shares}"
