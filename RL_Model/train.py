"""PPO self-training. Run with `python train.py`.

    python train.py                  # full run, Kaggle settings
    python train.py --preset smoke   # a couple of minutes, proves it works
    python train.py --resume         # pick up from the last checkpoint
    python train.py --init-from X.pt # start from trained weights, step 0

This is the training entry point and nothing else. `main.py` is the
tournament client and must stay free of torch, so the competition binary
never depends on a training framework being installed.

The PPO implementation here is ours: rollout collection, GAE, the clipped
objective and the update loop are all written out below rather than imported
from an RL library.

What it does *not* do yet is promote a trained checkpoint into play. The
evaluation below reports how the current policy fares against the heuristic
agent, and until that comparison is clearly favourable the heuristic remains
the agent that plays.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
from dataclasses import asdict
from pathlib import Path
from typing import Dict, List, Optional

import numpy as np
import torch
import torch.nn as nn

from config import PRESETS, TrainingConfig, board_range, shaping_at
from vvw import mapgen
from vvw.augment import augment_batch
from vvw.env import MAX_CANDIDATES, VvWEnv
from vvw.vecenv import make_vec_env
from vvw.network import PolicyValueNet, evaluate_actions

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

#: Below this many PPO updates a run is unlikely to go anywhere; see
#: `rollout_plan` for the measurement behind it.
MIN_HEALTHY_UPDATES = 150


def log(message: str) -> None:
    print(message, flush=True)


# ---------------------------------------------------------------------------
# Rollouts
# ---------------------------------------------------------------------------


def stack(observations: List[Dict[str, np.ndarray]]) -> Dict[str, torch.Tensor]:
    return {
        key: torch.as_tensor(
            np.stack([observation[key] for observation in observations])
        ).to(DEVICE, dtype=torch.float32)
        for key in ("board", "scalars", "candidates", "mask")
    }


class Rollout:
    """One batch of experience, flattened across environments."""

    def __init__(self):
        self.observations: List[Dict[str, np.ndarray]] = []
        self.actions: List[int] = []
        self.log_probs: List[float] = []
        self.values: List[float] = []
        self.rewards: List[float] = []
        self.dones: List[bool] = []

    def __len__(self) -> int:
        return len(self.actions)


def collect(
    vec,
    observations: List[Dict[str, np.ndarray]],
    network: PolicyValueNet,
    steps: int,
    shaping: float,
) -> tuple:
    """Run every environment forward `steps` times.

    `vec` is anything from `vvw.vecenv.make_vec_env`: a worker pool when
    `--workers` asks for one, a plain loop otherwise. Both reset an episode
    the moment it ends, so a finished game never stalls the batch.
    """
    rollout = Rollout()
    finished: List[float] = []
    vec.set_attr("shaping", shaping)

    for _ in range(steps):
        batch = stack(observations)
        actions, log_probs, values = network.act(batch)
        actions = actions.cpu().numpy()

        for index in range(vec.total):
            rollout.observations.append(observations[index])
            rollout.actions.append(int(actions[index]))
            rollout.log_probs.append(float(log_probs[index]))
            rollout.values.append(float(values[index]))

        observations, rewards, dones, _ = vec.step(
            [int(a) for a in actions]
        )
        for index in range(vec.total):
            rollout.rewards.append(rewards[index])
            rollout.dones.append(dones[index])
            if dones[index]:
                finished.append(rewards[index])

    with torch.no_grad():
        _, last_values = network(stack(observations))
    return rollout, observations, last_values.cpu().numpy(), finished


def advantages_and_returns(
    rollout: Rollout, last_values: np.ndarray, env_count: int, gamma: float, lam: float
):
    """Generalised advantage estimation, per environment.

    Entries are interleaved env-by-env, so the walk back has to stride by the
    number of environments to stay on one env's own trajectory.
    """
    steps = len(rollout) // env_count
    rewards = np.array(rollout.rewards, dtype=np.float32).reshape(steps, env_count)
    values = np.array(rollout.values, dtype=np.float32).reshape(steps, env_count)
    dones = np.array(rollout.dones, dtype=np.float32).reshape(steps, env_count)

    advantages = np.zeros_like(rewards)
    running = np.zeros(env_count, dtype=np.float32)
    next_values = last_values.astype(np.float32)

    for step in reversed(range(steps)):
        alive = 1.0 - dones[step]
        delta = rewards[step] + gamma * next_values * alive - values[step]
        running = delta + gamma * lam * alive * running
        advantages[step] = running
        next_values = values[step]

    returns = advantages + values
    return advantages.reshape(-1), returns.reshape(-1)


# ---------------------------------------------------------------------------
# Update
# ---------------------------------------------------------------------------


def update(
    network: PolicyValueNet,
    optimiser: torch.optim.Optimizer,
    rollout: Rollout,
    advantages: np.ndarray,
    returns: np.ndarray,
    cfg: TrainingConfig,
) -> Dict[str, float]:
    ppo = cfg.ppo
    observations = stack(rollout.observations)
    actions = torch.as_tensor(rollout.actions, dtype=torch.long, device=DEVICE)
    old_log_probs = torch.as_tensor(
        rollout.log_probs, dtype=torch.float32, device=DEVICE
    )
    advantage_tensor = torch.as_tensor(advantages, dtype=torch.float32, device=DEVICE)
    return_tensor = torch.as_tensor(returns, dtype=torch.float32, device=DEVICE)
    advantage_tensor = (advantage_tensor - advantage_tensor.mean()) / (
        advantage_tensor.std() + 1e-8
    )

    count = len(rollout)
    indices = np.arange(count)
    stats = {"policy_loss": 0.0, "value_loss": 0.0, "entropy": 0.0, "batches": 0}

    for _ in range(ppo.epochs_per_update):
        np.random.shuffle(indices)
        for start in range(0, count, ppo.minibatch_size):
            batch = indices[start : start + ppo.minibatch_size]
            if len(batch) < 2:
                continue
            batch_tensor = torch.as_tensor(batch, dtype=torch.long, device=DEVICE)
            minibatch = {key: value[batch_tensor] for key, value in observations.items()}

            log_probs, entropy, values = evaluate_actions(
                network, minibatch, actions[batch_tensor]
            )
            ratio = torch.exp(log_probs - old_log_probs[batch_tensor])
            batch_advantages = advantage_tensor[batch_tensor]

            unclipped = ratio * batch_advantages
            clipped = torch.clamp(ratio, 1 - ppo.clip, 1 + ppo.clip) * batch_advantages
            policy_loss = -torch.min(unclipped, clipped).mean()
            # Huber rather than squared error: returns here are dominated by
            # the terminal +/-1, so a value head that is badly wrong early in
            # training produces large residuals that a squared loss turns
            # into outsized gradients.
            value_loss = nn.functional.smooth_l1_loss(
                values, return_tensor[batch_tensor]
            )
            entropy_loss = entropy.mean()

            loss = (
                policy_loss
                + ppo.value_coef * value_loss
                - ppo.entropy_coef * entropy_loss
            )

            optimiser.zero_grad(set_to_none=True)
            loss.backward()
            nn.utils.clip_grad_norm_(network.parameters(), ppo.max_grad_norm)
            optimiser.step()

            stats["policy_loss"] += float(policy_loss.detach())
            stats["value_loss"] += float(value_loss.detach())
            stats["entropy"] += float(entropy_loss.detach())
            stats["batches"] += 1

    divisor = max(1, stats.pop("batches"))
    return {key: value / divisor for key, value in stats.items()}


# ---------------------------------------------------------------------------
# Is the policy actually moving?
# ---------------------------------------------------------------------------


class GreedyChurn:
    """Fraction of decisions whose best action changed since the last update.

    Entropy looked like the obvious health check and is a bad one. Measured:
    at lr 3e-3 the policy changed its chosen move on 47% of a fixed probe set
    in ten updates while entropy went from 2.28 to 2.25 -- all but unchanged.
    Entropy measures how spread the distribution is, not whether the argmax
    moved, and a policy can reorder its preferences without touching it.

    Churn near zero over many updates means PPO is not learning. Churn that
    stays very high means it is thrashing. Something in between is progress.
    """

    def __init__(self, split: str = "validation", size: int = 48):
        env = VvWEnv(split=split, shaping=0.0)
        self.states = []
        observation, info = env.reset(seed=20_001)
        guard = 0
        while len(self.states) < size and guard < size * 40:
            guard += 1
            if len(env._candidates) > 1:
                self.states.append((observation, len(env._candidates)))
            observation, _, terminated, truncated, info = env.step(
                info["heuristic_choice"]
            )
            if terminated or truncated:
                observation, info = env.reset(seed=20_001 + guard)
        self.previous: Optional[List[int]] = None

    @torch.no_grad()
    def measure(self, network: PolicyValueNet) -> Optional[float]:
        if not self.states:
            return None
        current = [
            int(network(stack([observation]))[0][0, :count].argmax())
            for observation, count in self.states
        ]
        churn = None
        if self.previous is not None:
            changed = sum(a != b for a, b in zip(self.previous, current))
            churn = changed / len(current)
        self.previous = current
        return churn


# ---------------------------------------------------------------------------
# Evaluation
# ---------------------------------------------------------------------------


@torch.no_grad()
def evaluate_against_heuristic(
    network: PolicyValueNet, cfg: TrainingConfig, maps: int
) -> Dict[str, float]:
    """Play the greedy policy against the heuristic on held-out maps.

    This is the go/no-go measurement. Until the policy is clearly ahead here,
    the heuristic is what plays in the tournament.

    Both species and both move orders are covered, because the opening side
    has a real advantage and a one-sided sample would measure that instead.
    Draws are counted as draws: equal creature counts at the cap is a genuine
    draw, and an agent has to win on creatures.
    """
    from vvw.agents import heuristic as heuristic_agent
    from vvw.state import VAMPIRES, WEREWOLVES, opponent_of

    env = VvWEnv(
        split="validation",
        opponents={"heuristic": heuristic_agent.choose_turn},
        shaping=0.0,
    )
    wins = losses = draws = 0
    margin = 0.0
    normalised = 0.0

    for index, seed in enumerate(mapgen.seeds("validation", maps)):
        for species in (VAMPIRES, WEREWOLVES):
            observation, _ = env.reset(
                seed=seed,
                options={
                    "map_seed": seed,
                    "species": species,
                    "opponent": "heuristic",
                    "we_move_first": index % 2 == 0,
                },
            )
            for _ in range(400):
                action, _, _ = network.act(stack([observation]), deterministic=True)
                observation, reward, terminated, truncated, _ = env.step(int(action[0]))
                if terminated or truncated:
                    wins += reward > 0
                    losses += reward < 0
                    draws += reward == 0
                    break
            mine = env.state.population(env.me)
            theirs = env.state.population(opponent_of(env.me))
            margin += mine - theirs
            # Bounded to [-1, 1] per game; see `_evaluate_and_promote`.
            normalised += (mine - theirs) / max(1, mine + theirs)

    played = max(1, wins + losses + draws)
    return {
        "games": played,
        "win_rate": wins / played,
        "loss_rate": losses / played,
        "draw_rate": draws / played,
        "mean_margin": margin / played,
        "mean_normalised_margin": normalised / played,
        "outcome_score": (wins - losses) / played,
    }


#: Mean decisions per game, measured over 40 games at each curriculum stage
#: with the training opponent mix: 10-20 boards 55.9, 10-35 68.1, 10-50 76.5.
#: Bigger boards take longer because the groups start further apart, and all
#: three sit under the 100-decisions-a-side cap.
MEAN_GAME_LENGTH = {20: 55.9, 35: 68.1, 50: 76.5}


def mean_game_length(cfg: TrainingConfig) -> float:
    """Decisions per game averaged over the run, weighted by the curriculum.

    Each stage holds until the next threshold, so a run that spends 15% of
    itself on small boards and the rest on wide ones is mostly priced at the
    wide-board length.
    """
    stages = list(cfg.curriculum)
    total = 0.0
    for index, (threshold, _low, high) in enumerate(stages):
        end = stages[index + 1][0] if index + 1 < len(stages) else 1.0
        nearest = min(MEAN_GAME_LENGTH, key=lambda side: abs(side - high))
        total += (end - threshold) * MEAN_GAME_LENGTH[nearest]
    return total


def games_plan(
    cfg: TrainingConfig,
    distill_iterations: int,
    distill_envs: int = 8,
    distill_steps: int = 24,
) -> List[str]:
    """How many games the run will actually play, before it starts.

    `total_steps` counts agent decisions, not games, and the two differ by
    almost two orders of magnitude. A run sized in steps is easy to
    misjudge: 200k steps sounds large and is about three thousand games,
    which is small for self-play. This prints the games so the size of the
    run is visible at the point where it can still be changed.
    """
    length = mean_game_length(cfg)
    distill_transitions = distill_iterations * distill_envs * distill_steps
    evaluations = cfg.total_steps // max(1, cfg.evaluate_every) + 1
    eval_games = evaluations * cfg.evaluation_maps * 2
    return [
        f"games: ~{length:.0f} decisions each -> "
        f"~{round(distill_transitions / length):,} distillation + "
        f"~{round(cfg.total_steps / length):,} PPO "
        f"= ~{round((distill_transitions + cfg.total_steps) / length):,} played",
        f"       {eval_games:,} more against the heuristic "
        f"({evaluations} evaluations x {cfg.evaluation_maps} maps x 2 sides)",
    ]


def take_snapshot(network: PolicyValueNet) -> Dict[str, "torch.Tensor"]:
    """A detached CPU copy of the weights, for the self-play pool.

    CPU because the snapshot is pickled out to worker processes that have no
    GPU context of their own, and detached-and-cloned because the live
    network keeps training: a view onto its parameters would make every
    sparring partner silently become the current policy.
    """
    return {
        name: tensor.detach().to("cpu").clone()
        for name, tensor in network.state_dict().items()
    }


def rollout_plan(cfg: TrainingConfig) -> List[str]:
    """Describe how the rollout size turns into PPO updates, and warn if few.

    One update consumes `parallel_envs * steps_per_env` transitions, and PPO
    improves per update rather than per step. Raising the environment count
    without lowering the rollout length therefore buys bigger, rarer updates:
    96 environments at the default 128 gives 81 updates across a million
    steps, where 8 environments gave 293 across three hundred thousand. A run
    that needed about 100 updates to reach its peak would never get there.
    """
    per_update = cfg.parallel_envs * cfg.steps_per_env
    updates = cfg.total_steps // max(1, per_update)
    lines = [
        f"rollout: {cfg.parallel_envs} envs x {cfg.steps_per_env} steps "
        f"= {per_update:,} per update -> {updates} updates"
    ]
    if per_update < cfg.ppo.minibatch_size:
        lines.append(
            f"[warn] a batch of {per_update:,} is smaller than minibatch_size "
            f"({cfg.ppo.minibatch_size}), so each update makes only "
            f"{cfg.ppo.epochs_per_update} gradient steps on one noisy batch. "
            "Raise --steps-per-env or --parallel-envs until envs x "
            "steps-per-env is around 1024-2048."
        )
    if updates < MIN_HEALTHY_UPDATES:
        suggestion = max(1, cfg.steps_per_env // 4)
        lines.append(
            f"[warn] only {updates} PPO updates. Lower --steps-per-env to "
            f"about {suggestion}, or use fewer environments: a previous run "
            "needed roughly 100 updates just to reach its peak."
        )
    return lines


def _evaluate_and_promote(
    network: PolicyValueNet,
    cfg: TrainingConfig,
    output: Path,
    step: int,
    history: List[Dict],
    best_score: float,
    label: str,
) -> float:
    """Measure, log, and keep the best network seen so far.

    Selection is on the **normalised** margin: the per-game creature lead
    divided by the creatures on the board, averaged, so every game
    contributes something in [-1, 1].

    It used to be the raw margin, justified by raw margin having "more
    statistical power per game" than win rate. Measured over 120 games, that
    was wrong: raw margin, normalised margin and the win-loss score all carry
    exactly the same 1.96/sqrt(n) noise relative to their own spread. What
    separates them is outlier sensitivity. Raw per-game margins reach +/-294
    on a wipe-out, so one blow-out moves a 120-game mean by 2.5 against a
    95% interval of 9.3 -- a quarter of the whole interval from a single
    game. Selecting `best.pt` on that is largely selecting which evaluation
    drew the lucky blow-out, and it is the most likely reason a checkpoint
    measured +3.1 on validation and -5.5 on test.

    The bounded score cannot be dominated that way: no game can move the
    mean by more than 1/n. The raw margin and the win-loss score are still
    reported, because they are what a reader wants to see.
    """
    report = evaluate_against_heuristic(network, cfg, cfg.evaluation_maps)
    report["step"] = step
    log(f"{label} " + json.dumps(report))
    history.append({"evaluation": report})

    score = report["mean_normalised_margin"]
    if score > best_score:
        torch.save(
            {"network": network.state_dict(), "step": step, "evaluation": report},
            output / "best.pt",
        )
        return score
    return best_score


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", default="default", choices=sorted(PRESETS))
    parser.add_argument("--resume", action="store_true")
    parser.add_argument(
        "--init-from",
        help="seed the network from a saved checkpoint and train on from "
        "there. Unlike --resume this needs only the weights, so it accepts "
        "a best.pt, which carries no optimiser state. The optimiser and the "
        "step counter start fresh, and distillation is skipped because the "
        "weights already contain it.",
    )
    parser.add_argument("--output", default=None)
    parser.add_argument(
        "--parallel-envs",
        type=int,
        default=None,
        help="environments stepped together; this is the batch size handed "
        "to the network and on its own buys no throughput",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=1,
        help="worker processes stepping environments. THIS is the throughput "
        "lever: measured on 12 cores, 25 env-steps/s in one process against "
        "147 with 12 workers. Set it to roughly your core count.",
    )
    parser.add_argument(
        "--steps-per-env",
        type=int,
        default=None,
        help="rollout length per environment. With many environments this "
        "must come DOWN: PPO improves per update, not per step, and "
        "envs x steps-per-env is one update's worth of experience.",
    )
    parser.add_argument(
        "--total-steps",
        type=int,
        default=None,
        help="override the preset's step budget, for a short measured run",
    )
    parser.add_argument(
        "--distill-iterations",
        type=int,
        default=40,
        help="supervised warm-start passes before PPO; 0 skips it",
    )
    args = parser.parse_args(argv)

    cfg = PRESETS[args.preset]
    if args.total_steps is not None:
        from dataclasses import replace as _replace

        # A short run would otherwise never reach the preset's checkpoint or
        # evaluation interval, and would finish with nothing saved along the
        # way and no measurement until the very end.
        cfg = _replace(
            cfg,
            total_steps=args.total_steps,
            checkpoint_every=min(cfg.checkpoint_every, max(1, args.total_steps // 4)),
            evaluate_every=min(cfg.evaluate_every, max(1, args.total_steps // 2)),
        )
    if args.parallel_envs is not None:
        from dataclasses import replace as _replace2

        cfg = _replace2(cfg, parallel_envs=args.parallel_envs)
    if args.steps_per_env is not None:
        from dataclasses import replace as _replace3

        cfg = _replace3(cfg, steps_per_env=args.steps_per_env)

    for line in rollout_plan(cfg):
        log(line)
    for line in games_plan(cfg, args.distill_iterations):
        log(line)

    output = Path(args.output or cfg.output_dir)
    output.mkdir(parents=True, exist_ok=True)

    torch.manual_seed(cfg.seed)
    np.random.seed(cfg.seed)
    random.seed(cfg.seed)

    network = PolicyValueNet(
        channels=cfg.network.channels,
        blocks=cfg.network.blocks,
        kernel=cfg.network.kernel,
        head_hidden=cfg.network.head_hidden,
    ).to(DEVICE)
    optimiser = torch.optim.Adam(network.parameters(), lr=cfg.ppo.learning_rate)

    step = 0
    checkpoint_path = output / "checkpoint.pt"
    if args.resume and checkpoint_path.exists():
        checkpoint = torch.load(checkpoint_path, map_location=DEVICE)
        network.load_state_dict(checkpoint["network"])
        optimiser.load_state_dict(checkpoint["optimiser"])
        step = checkpoint["step"]
        log(f"resumed from {checkpoint_path} at step {step}")
    elif args.init_from:
        # `best.pt` holds {network, step, evaluation} and no optimiser, so it
        # cannot go through --resume. Taking the weights alone is enough to
        # continue improving a policy; the optimiser moments are worth little
        # once the run restarts against a fresh rollout distribution anyway.
        seed_checkpoint = torch.load(args.init_from, map_location=DEVICE)
        network.load_state_dict(seed_checkpoint["network"])
        trained_to = seed_checkpoint.get("step", "unknown")
        log(
            f"initialised from {args.init_from} (trained to step {trained_to}); "
            "optimiser and step counter start at zero"
        )

    if args.distill_iterations and step == 0 and not args.init_from:
        distill(network, cfg, args.distill_iterations, workers=args.workers)

    vec = make_vec_env(
        total=cfg.parallel_envs,
        workers=max(1, min(args.workers, cfg.parallel_envs)),
        env_kwargs={
            "split": "train",
            "shaping": cfg.shaping,
            "opponent_mix": cfg.opponent_mix,
        },
    )
    if cfg.snapshot_every:
        # The workers need the shape before they can rebuild any weights.
        vec.set_attr(
            "network_config",
            {
                "channels": cfg.network.channels,
                "blocks": cfg.network.blocks,
                "head_hidden": cfg.network.head_hidden,
            },
        )
    observations, _ = vec.reset([cfg.seed + i for i in range(cfg.parallel_envs)])

    log(
        f"preset={cfg.name} device={DEVICE} envs={cfg.parallel_envs} "
        f"workers={vec.workers} "
        f"params={sum(p.numel() for p in network.parameters()):,}"
    )
    started = time.time()
    history: List[Dict] = []
    best_score = float("-inf")
    churn_probe = GreedyChurn()
    churn_probe.measure(network)  # establish the baseline before any update
    last_checkpoint = last_evaluation = last_snapshot = step
    snapshots: List[Dict[str, "torch.Tensor"]] = []
    #: Whether an evaluation has promoted a new best since the last snapshot.
    promoted = False

    while step < cfg.total_steps:
        elapsed = time.time() - started
        if elapsed > cfg.wall_clock_limit_seconds:
            log(f"stopping at the wall-clock limit after {elapsed / 3600:.2f} h")
            break

        vec.set_attr("board_range", board_range(cfg, step))

        rollout, observations, last_values, finished = collect(
            vec, observations, network, cfg.steps_per_env, shaping_at(cfg, step)
        )
        advantages, returns = advantages_and_returns(
            rollout, last_values, vec.total, cfg.ppo.gamma, cfg.ppo.gae_lambda
        )
        stats = update(network, optimiser, rollout, advantages, returns, cfg)
        step += len(rollout)


        churn = churn_probe.measure(network)
        record = {
            "step": step,
            "elapsed_s": round(time.time() - started, 1),
            # The health check that actually works; see GreedyChurn.
            "greedy_churn": None if churn is None else round(churn, 3),
            "episodes_finished": len(finished),
            "mean_terminal_reward": (
                round(sum(finished) / len(finished), 3) if finished else None
            ),
            **{key: round(value, 4) for key, value in stats.items()},
        }
        history.append(record)
        log(json.dumps(record))

        if step - last_checkpoint >= cfg.checkpoint_every:
            last_checkpoint = step
            torch.save(
                {
                    "network": network.state_dict(),
                    "optimiser": optimiser.state_dict(),
                    "step": step,
                    "config": asdict(cfg),
                },
                checkpoint_path,
            )

        if step - last_evaluation >= cfg.evaluate_every:
            last_evaluation = step
            previous_best = best_score
            best_score = _evaluate_and_promote(
                network, cfg, output, step, history, best_score, "eval"
            )
            promoted = promoted or best_score > previous_best

        # After the evaluation on purpose: a snapshot taken before one is a
        # network nobody has measured.
        if cfg.snapshot_every and step - last_snapshot >= cfg.snapshot_every:
            last_snapshot = step
            if cfg.snapshot_requires_improvement and not promoted:
                # The guard that matters for self-play. Without it a policy
                # that is getting worse keeps feeding worse sparring
                # partners into the pool, and then trains against them --
                # the collapse reinforces itself. A snapshot has to have
                # earned its place by beating every evaluation before it.
                log(f"snapshot at step {step}: skipped, nothing promoted since the last one")
            else:
                snapshots.append(take_snapshot(network))
                # Keep the newest few. Beating a network from long ago stops
                # being evidence of anything fairly quickly.
                del snapshots[: -cfg.snapshot_pool]
                vec.set_attr("snapshots", snapshots)
                log(f"snapshot at step {step}: pool holds {len(snapshots)}")
            promoted = False

    torch.save(
        {
            "network": network.state_dict(),
            "optimiser": optimiser.state_dict(),
            "step": step,
            "config": asdict(cfg),
        },
        checkpoint_path,
    )
    # The final evaluation promotes too. It used to only log, so a run whose
    # last measurement was its best left `best.pt` holding an earlier, worse
    # network -- one real run shipped a step-100k model scoring 0.375 while
    # the step-300k model it finished with scored 0.45.
    best_score = _evaluate_and_promote(
        network, cfg, output, step, history, best_score, "final eval"
    )

    vec.close()
    (output / "history.json").write_text(json.dumps(history, indent=2), encoding="utf-8")
    log(f"done: {step} steps in {(time.time() - started) / 60:.1f} min -> {output}")
    return 0




# ---------------------------------------------------------------------------
# Warm start
# ---------------------------------------------------------------------------


def distill(
    network: PolicyValueNet,
    cfg: TrainingConfig,
    iterations: int,
    batch_envs: int = 8,
    steps_per_env: int = 24,
    augment: bool = False,
    workers: int = 1,
) -> Dict[str, float]:
    """Teach the policy to reproduce the heuristic's choice, from features.

    PPO starting from a uniform policy over sixteen candidates has to find the
    good ones by trial, and the only unambiguous reward arrives once per
    hundred-odd steps at the end of a game. Starting it from a policy that
    already plays about as well as the heuristic means PPO spends its samples
    on *improving* that policy rather than rediscovering it.

    This is supervised learning, not RL, and it is only possible because the
    candidates are shuffled: the target is "the candidate the heuristic would
    pick", which has to be recognised from the candidate's own features. If
    they arrived sorted, the answer would always be index 0 and the network
    would learn nothing worth having.

    The ceiling is the heuristic. Everything above it has to come from PPO.
    """
    optimiser = torch.optim.Adam(network.parameters(), lr=1e-3)
    # The warm start is environment-bound exactly like PPO is, so it gets the
    # same worker pool. At 3.6 s an iteration single-process, a few hundred
    # iterations is otherwise a chunk of the run spent on one core.
    vec = make_vec_env(
        total=batch_envs,
        workers=max(1, min(workers, batch_envs)),
        env_kwargs={"split": "train", "shaping": 0.0},
    )
    observations, infos = vec.reset(
        [cfg.seed + 1000 + index for index in range(batch_envs)]
    )
    targets = [info["heuristic_choice"] for info in infos]

    history = {"loss": 0.0, "accuracy": 0.0}
    for iteration in range(iterations):
        batch_observations, batch_targets = [], []

        for _ in range(steps_per_env):
            batch_observations.extend(observations)
            batch_targets.extend(targets)
            # Teacher forcing: follow the heuristic so the states we learn on
            # are the ones a competent player actually reaches.
            observations, _, _, infos = vec.step(targets)
            targets = [info["heuristic_choice"] for info in infos]

        if augment:
            # Off by default: measured to be a no-op here. See
            # `vvw/augment.py` for why -- this trunk is already invariant to
            # reorientation, so the augmented sample produces identical
            # logits. Kept switchable for a future spatial head.
            boards = augment_batch(
                np.stack([o["board"] for o in batch_observations])
            )
            batch_observations = [
                {**o, "board": boards[i]} for i, o in enumerate(batch_observations)
            ]

        batch = stack(batch_observations)
        labels = torch.as_tensor(batch_targets, dtype=torch.long, device=DEVICE)
        logits, _ = network(batch)
        loss = nn.functional.cross_entropy(logits, labels)

        optimiser.zero_grad(set_to_none=True)
        loss.backward()
        nn.utils.clip_grad_norm_(network.parameters(), cfg.ppo.max_grad_norm)
        optimiser.step()

        with torch.no_grad():
            accuracy = float((logits.argmax(dim=1) == labels).float().mean())
            # What a policy that guesses uniformly would score, so the number
            # above can be read as "better than chance" or not.
            chance = float((1.0 / batch["mask"].sum(dim=1)).mean())
        # detach: float() on a tensor that still carries grad warns, and the
        # value here is only for the log.
        history = {
            "loss": float(loss.detach()),
            "accuracy": accuracy,
            "chance": chance,
        }
        if iteration % 5 == 0 or iteration == iterations - 1:
            log(
                "distill "
                + json.dumps({"iteration": iteration, **{
                    k: round(v, 4) for k, v in history.items()
                }})
            )
    vec.close()
    return history


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
