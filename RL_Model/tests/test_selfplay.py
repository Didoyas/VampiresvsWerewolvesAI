"""Self-play: snapshots of the learner as sparring partners.

Kept separate from the measured `default` run on purpose, so these tests
also pin down that separation.
"""

import pytest

from config import PRESETS
from vvw.env import SELFPLAY, VvWEnv

torch = pytest.importorskip("torch")

from train import take_snapshot
from vvw.network import PolicyValueNet

SHAPE = {"channels": 8, "blocks": 2, "head_hidden": 32}


def _snapshots(count=3):
    return [PolicyValueNet(**SHAPE).state_dict() for _ in range(count)]


def _env(snapshots=None):
    env = VvWEnv(split="train")
    env.board_range = (10, 14)
    env.network_config = dict(SHAPE)
    if snapshots is not None:
        env.snapshots = snapshots
    return env


# ---- the old version must stay the old version -----------------------------


def test_the_default_preset_has_no_self_play():
    """`default` is the configuration every past measurement used."""
    assert PRESETS["default"].snapshot_every == 0
    assert SELFPLAY not in PRESETS["default"].opponent_mix


def test_the_selfplay_preset_is_a_separate_entry():
    cfg = PRESETS["selfplay"]
    assert cfg.name == "selfplay"
    assert cfg.snapshot_every > 0
    assert cfg.opponent_mix[SELFPLAY] > 0
    assert "heuristic" in cfg.opponent_mix, "a fixed yardstick has to stay in the pool"


def test_evaluation_uses_enough_maps_to_be_representative():
    """20 maps put +3.1 on validation against -5.5 on test for one
    checkpoint, because the first 20 validation maps are drawish."""
    assert PRESETS["default"].evaluation_maps >= 60


# ---- snapshots -------------------------------------------------------------


def test_a_snapshot_does_not_follow_the_network_as_it_trains():
    network = PolicyValueNet(**SHAPE)
    snapshot = take_snapshot(network)
    with torch.no_grad():
        for parameter in network.parameters():
            parameter.add_(1.0)
    for name, tensor in snapshot.items():
        if tensor.is_floating_point():
            assert not torch.equal(tensor, network.state_dict()[name].cpu())


def test_a_snapshot_is_on_the_cpu_for_the_workers():
    for tensor in take_snapshot(PolicyValueNet(**SHAPE)).values():
        assert tensor.device.type == "cpu"


# ---- the pool --------------------------------------------------------------


def test_selfplay_is_only_offered_once_a_snapshot_exists():
    empty, stocked = _env(), _env(_snapshots())
    assert SELFPLAY not in empty._available_opponents()
    assert SELFPLAY in stocked._available_opponents()


def test_asking_for_selfplay_too_early_falls_back_instead_of_crashing():
    env = _env()
    _, info = env.reset(seed=1, options={"opponent": SELFPLAY})
    assert info["opponent"] == "heuristic"


def test_the_sparring_partner_holds_still_for_a_whole_game():
    """A snapshot drawn per turn is not an opponent, it is noise."""
    env = _env(_snapshots())
    env.reset(seed=3, options={"opponent": SELFPLAY})
    chosen = env._snapshot_index
    for _ in range(25):
        if env.step(0)[2]:
            break
        assert env._snapshot_index == chosen


def test_different_episodes_draw_different_snapshots():
    env = _env(_snapshots())
    drawn = set()
    for seed in range(30):
        env.reset(seed=seed, options={"opponent": SELFPLAY})
        drawn.add(env._snapshot_index)
    assert len(drawn) > 1


def test_a_new_pool_discards_the_cached_agents():
    env = _env(_snapshots())
    env.reset(seed=1, options={"opponent": SELFPLAY})
    env.step(0)
    assert env._snapshot_agents, "an agent should have been built and kept"
    env.snapshots = _snapshots()
    assert env._snapshot_agents == {}, "stale agents would play the old weights"


def test_a_game_against_a_snapshot_runs_to_a_terminal_reward():
    env = _env(_snapshots())
    env.reset(seed=5, options={"opponent": SELFPLAY})
    for _ in range(400):
        _, reward, terminated, _, _ = env.step(0)
        if terminated:
            assert reward in (-1.0, 0.0, 1.0)
            break
    else:
        pytest.fail("the game never ended")


def test_the_snapshot_opponent_only_plays_legal_turns():
    from vvw.rules import validate_turn
    from vvw.state import opponent_of

    env = _env(_snapshots(1))
    env.reset(seed=9, options={"opponent": SELFPLAY})
    agent = env._agent_for(SELFPLAY)
    for _ in range(12):
        them = opponent_of(env.me)
        if not env.state.groups(them):
            break
        turn = agent(env.state, them)
        if turn:
            assert validate_turn(env.state, turn, them, env.unknowns) == []
        if env.step(0)[2]:
            break


# ---- the whole loop, with self-play switched on ----------------------------


def _tiny(**overrides):
    from dataclasses import replace

    from config import NetworkConfig

    settings = dict(
        total_steps=256,
        parallel_envs=2,
        steps_per_env=16,
        snapshot_every=64,
        snapshot_pool=2,
        checkpoint_every=10_000,
        evaluate_every=10_000,
        evaluation_maps=2,
        curriculum=((0.0, 10, 12),),
        network=NetworkConfig(channels=8, blocks=2, head_hidden=32),
    )
    settings.update(overrides)
    return replace(PRESETS["selfplay"], **settings)


def _run(cfg, tmp_path, capsys):
    import train

    with pytest.MonkeyPatch.context() as patch:
        patch.setitem(train.PRESETS, "selfplay", cfg)
        assert (
            train.main(
                [
                    "--preset", "selfplay",
                    "--output", str(tmp_path / "run"),
                    "--distill-iterations", "0",
                ]
            )
            == 0
        )
    return capsys.readouterr().out


def _pool_sizes(output):
    return [
        int(line.rsplit(" ", 1)[1])
        for line in output.splitlines()
        if "pool holds" in line
    ]


def test_ungated_snapshots_fill_the_pool_and_cap_it(tmp_path, capsys):
    """The cadence itself works: taken on schedule, trimmed to the cap."""
    sizes = _pool_sizes(_run(_tiny(snapshot_requires_improvement=False), tmp_path, capsys))
    assert sizes, "no snapshot was ever taken"
    assert max(sizes) == 2, f"pool was not capped: {sizes}"
    assert sizes == sorted(sizes), f"pool should grow then hold: {sizes}"


def test_the_gate_refuses_snapshots_that_no_evaluation_has_promoted(tmp_path, capsys):
    """The protection against a self-play pool poisoning itself.

    With no evaluation inside the loop nothing can be promoted, so nothing
    may enter the pool -- an unmeasured network is not a sparring partner.
    """
    output = _run(_tiny(), tmp_path, capsys)
    assert _pool_sizes(output) == [], "an unpromoted network entered the pool"
    assert "skipped, nothing promoted" in output


def test_a_promoted_network_does_enter_the_pool(tmp_path, capsys):
    """And the gate is not simply blocking everything: the first evaluation
    always promotes, because the best score starts at minus infinity."""
    sizes = _pool_sizes(_run(_tiny(evaluate_every=64), tmp_path, capsys))
    assert sizes, "a promoted network should have entered the pool"
    assert max(sizes) <= 2
