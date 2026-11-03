"""Every tunable in one place, with presets.

    from config import PRESETS
    cfg = PRESETS["smoke"]      # minutes, tiny, proves the pipeline runs
    cfg = PRESETS["default"]    # a full Kaggle session

These values are starting points, not tuned optima. Nothing here has been
shown to be better than its neighbours; they are conventional PPO settings
scaled to a small model, and the comments say what each one is for so they
can be moved on evidence rather than taste.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from typing import Dict, Tuple


@dataclass(frozen=True)
class NetworkConfig:
    """A small fully convolutional trunk with policy and value heads.

    Fully convolutional so one network handles every board from 10x10 to
    50x50. Global pooling feeds the heads, and the whole-board scalars from
    `vvw.features` are concatenated there, because pooling discards the
    absolute populations that decide the game.
    """

    channels: int = 32
    blocks: int = 4
    kernel: int = 3
    head_hidden: int = 128


@dataclass(frozen=True)
class PPOConfig:
    #: Measured on a fixed probe set, ten updates each: at 3e-4 the policy
    #: changed its chosen move on 8% of decisions, at 3e-3 on 47%, and at 1e-2
    #: on 23% -- too high and the clip fights every step. Two full runs looked
    #: dead at the old default, and the reason was simply this number.
    learning_rate: float = 3e-3
    #: Terminal reward is +/-1 at the end of a long game, so discounting
    #: hard would hide it; near 1 keeps it visible back through the episode.
    gamma: float = 0.997
    gae_lambda: float = 0.95
    clip: float = 0.2
    #: Left at the conventional value on purpose. Ablated at lr 3e-3 over ten
    #: updates: 0.0 gave 47% greedy churn, 0.003 gave 42%, 0.01 gave 57% --
    #: no ordering, just noise. The learning rate was the whole story (1e-3
    #: gave 10% at the same coefficient), so this knob stays put.
    #: Raised from 0.01 after a 293-update run collapsed: entropy fell from
    #: 2.22 to 0.29 -- nearly deterministic over sixteen candidates -- and
    #: greedy churn hit exactly zero for the last 26 updates, so the policy
    #: stopped changing 27k steps before the run ended. An earlier ablation
    #: found no effect from this coefficient, but it only ran ten updates,
    #: which cannot see a collapse that takes hundreds. This value is a
    #: response to that evidence, not a measured optimum: watch entropy and
    #: churn over a long run and move it again.
    entropy_coef: float = 0.03
    value_coef: float = 0.5
    max_grad_norm: float = 0.5
    epochs_per_update: int = 4
    minibatch_size: int = 256


@dataclass(frozen=True)
class TrainingConfig:
    name: str = "default"
    seed: int = 0
    parallel_envs: int = 16
    steps_per_env: int = 128
    total_steps: int = 4_000_000

    #: Dense reward on progress, decayed to zero so the final policy
    #: optimises the actual win condition and not the proxy.
    #: Measured: at 0.05, 75% of steps carried an advantage below 0.01 and
    #: PPO had almost nothing to learn from. At 0.3 that falls to 14%. Higher
    #: still works but risks the proxy drowning the terminal +/-1.
    shaping: float = 0.3
    shaping_decay_steps: int = 500_000

    #: Boards start small so early games are short and the terminal reward
    #: arrives often, then widen to the full tournament range.
    #:
    #: Expressed as a FRACTION of the run, not an absolute step count. The
    #: thresholds used to be absolute and sized for a four-million-step run,
    #: so a 300k run never saw a board wider than 20 and a one-million-step
    #: run never passed 35 -- while the tournament draws uniformly from
    #: 10x10 to 50x50. Most of the range was never trained on.
    curriculum: Tuple[Tuple[float, int, int], ...] = (
        (0.00, 10, 20),
        (0.15, 10, 35),
        (0.35, 10, 50),
    )

    opponent_mix: Dict[str, float] = field(
        default_factory=lambda: {"random": 0.15, "naive": 0.35, "heuristic": 0.50}
    )

    #: Self-play. Off in `default`, which stays exactly the run that has
    #: been measured so far; `PRESETS["selfplay"]` is the version that
    #: trains against copies of itself. Zero disables snapshots entirely.
    snapshot_every: int = 0
    #: How many past versions to spar against. A pool rather than the latest
    #: network alone: training only against your current self is how a policy
    #: chases its own tail, drifting into lines that beat today's weights and
    #: nothing else.
    snapshot_pool: int = 6
    #: Only admit a snapshot if an evaluation has promoted a new best since
    #: the previous one. This is the guard against the pool poisoning
    #: itself: a degrading policy would otherwise keep supplying weaker
    #: sparring partners, which it then trains against.
    snapshot_requires_improvement: bool = True

    checkpoint_every: int = 50_000
    evaluate_every: int = 100_000
    #: Raised from 20 after the 20-map validation set turned out to be
    #: unrepresentative: the heuristic playing itself drew 82.5% of games on
    #: those maps against 29% on the test split, and only 7 of 40 games had a
    #: non-zero margin. `best.pt` was being chosen on that handful, and the
    #: checkpoint it picked measured +3.1 on validation and -5.5 on test.
    evaluation_maps: int = 60
    #: Kaggle sessions are cut off near 9 h; stop early and save cleanly.
    wall_clock_limit_seconds: int = int(8.5 * 3600)
    output_dir: str = "/kaggle/working"

    network: NetworkConfig = field(default_factory=NetworkConfig)
    ppo: PPOConfig = field(default_factory=PPOConfig)


DEFAULT = TrainingConfig()

SMOKE = replace(
    DEFAULT,
    name="smoke",
    parallel_envs=2,
    steps_per_env=16,
    total_steps=2_000,
    checkpoint_every=1_000,
    evaluate_every=1_000,
    evaluation_maps=2,
    wall_clock_limit_seconds=300,
    output_dir="runs/smoke",
    curriculum=((0, 10, 12),),
    network=NetworkConfig(channels=8, blocks=2, head_hidden=32),
)

#: The self-play version, kept separate on purpose: `default` is the run
#: whose numbers we have, and changing it would make every past measurement
#: incomparable. Snapshots of the learner join the opponent pool as training
#: goes on, so the policy has to keep beating what it used to be instead of
#: settling for whatever beats the heuristic.
SELFPLAY = replace(
    DEFAULT,
    name="selfplay",
    snapshot_every=50_000,
    snapshot_pool=6,
    #: The scripted agents stay in the mix. Pure self-play drifts: both
    #: sides can agree on a bad plan and neither is punished for it, and the
    #: heuristic is the only fixed yardstick in the pool.
    opponent_mix={
        "random": 0.05,
        "naive": 0.15,
        "heuristic": 0.40,
        "selfplay": 0.40,
    },
)

PRESETS: Dict[str, TrainingConfig] = {
    "default": DEFAULT,
    "smoke": SMOKE,
    "selfplay": SELFPLAY,
}


def board_range(cfg: TrainingConfig, step: int) -> Tuple[int, int]:
    """The board sizes the curriculum calls for at this point in training.

    Thresholds are fractions of `total_steps`, so a short run still reaches
    the full tournament range instead of spending all of itself on small
    boards.
    """
    progress = step / max(1, cfg.total_steps)
    low, high = cfg.curriculum[0][1], cfg.curriculum[0][2]
    for threshold, smallest, largest in cfg.curriculum:
        if progress >= threshold:
            low, high = smallest, largest
    return low, high


def shaping_at(cfg: TrainingConfig, step: int) -> float:
    """Shaping weight, decayed linearly to zero."""
    if cfg.shaping_decay_steps <= 0:
        return cfg.shaping
    remaining = max(0.0, 1.0 - step / cfg.shaping_decay_steps)
    return cfg.shaping * remaining
