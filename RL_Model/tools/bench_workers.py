"""Find the worker count that actually goes fastest on this machine.

    python tools/bench_workers.py

More workers is not automatically faster, and the right number is a property
of the machine rather than of the code. One 12-core laptop measured 271
steps/sec at 6 workers and 287 at 12, while a 64-core server running the same
code managed 17 -- so the only way to pick is to measure where you are.

Run this before a long training run and pass the winner to
`train.py --workers`.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def measure(workers: int, envs: int, steps: int = 6) -> float:
    import torch

    from config import PRESETS
    from train import stack
    from vvw.network import PolicyValueNet
    from vvw.vecenv import make_vec_env

    cfg = PRESETS["default"]
    net = PolicyValueNet(
        channels=cfg.network.channels,
        blocks=cfg.network.blocks,
        head_hidden=cfg.network.head_hidden,
    )
    vec = make_vec_env(
        total=envs,
        workers=workers,
        env_kwargs={"split": "train", "shaping": cfg.shaping},
    )
    try:
        observations, _ = vec.reset(list(range(envs)))
        for _ in range(2):  # warm up, and pay the worker start-up once
            with torch.no_grad():
                actions = net.act(stack(observations))[0].cpu().numpy()
            observations, _, _, _ = vec.step([int(a) for a in actions])
        started = time.time()
        for _ in range(steps):
            with torch.no_grad():
                actions = net.act(stack(observations))[0].cpu().numpy()
            observations, _, _, _ = vec.step([int(a) for a in actions])
        return steps * envs / (time.time() - started)
    finally:
        vec.close()


def main() -> int:
    cores = os.cpu_count() or 1
    print(f"{cores} cores detected\n")
    plans = [(1, 8), (4, 8), (8, 16), (16, 32)]
    for extra in (cores // 4, cores // 2, cores):
        if extra > 16:
            plans.append((extra, extra * 2))
    seen, best = set(), (0.0, None)
    print(f"{'workers':>8} {'envs':>6} {'steps/sec':>11}  {'1M steps':>10}")
    for workers, envs in plans:
        if (workers, envs) in seen:
            continue
        seen.add((workers, envs))
        rate = measure(workers, envs)
        hours = 1_000_000 / rate / 3600
        print(f"{workers:>8} {envs:>6} {rate:>11.0f}  {hours:>9.1f}h", flush=True)
        if rate > best[0]:
            best = (rate, (workers, envs))
    print(f"\nfastest here: --workers {best[1][0]} --parallel-envs {best[1][1]}")
    print(f"that is {1_000_000/best[0]/3600:.1f} hours for a million steps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
