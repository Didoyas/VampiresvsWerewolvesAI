"""Environments stepped in worker processes, so more cores mean more throughput.

`VvWEnv.step` is expensive and entirely Python: it runs the heuristic's
candidate generator and scores up to sixteen candidate turns. The network, by
comparison, is 78k parameters and barely registers. So the environment is the
bottleneck, and the way to go faster is to step many of them at once on
different cores.

Before this existed, `train.py` stepped its environments in a plain loop in
one process. Measured, that gave a flat **23-30 environment steps per second
regardless of how many environments were configured** -- two environments or
thirty-two, the same throughput, with the wall clock per update simply
growing in proportion. `parallel_envs` was a batch-size knob wearing the name
of a parallelism knob, and raising it on a many-core machine bought nothing.

Workers are started with `spawn`, which is the only option on Windows, so
nothing environment-shaped is ever pickled: each worker is told how to build
its own environments and does so after the fork.

**Anything that constructs this must be guarded by `if __name__ ==
"__main__":`.** Under `spawn` each worker re-imports the module that started
it, so a pool created at module level starts a pool inside every worker, and
that recursion forks until the machine gives up. `train.py` has the guard;
a throwaway benchmark script did not, and buried the machine in processes.
"""

from __future__ import annotations

import multiprocessing as mp
from typing import Any, Dict, List, Optional, Sequence, Tuple

import numpy as np


def _worker(remote, parent_remote, count: int, kwargs: Dict[str, Any]) -> None:
    """Own `count` environments and serve commands until told to close."""
    parent_remote.close()
    from .env import VvWEnv  # imported here so the parent never pickles it

    envs = [VvWEnv(**kwargs) for _ in range(count)]
    try:
        while True:
            command, payload = remote.recv()

            if command == "reset":
                out = []
                for env, seed in zip(envs, payload):
                    observation, info = env.reset(seed=seed)
                    out.append((observation, info))
                remote.send(out)

            elif command == "step":
                out = []
                for env, action in zip(envs, payload):
                    observation, reward, terminated, truncated, info = env.step(action)
                    if terminated or truncated:
                        # Reset in the worker, so one finished episode never
                        # stalls the whole batch waiting on the main process.
                        observation, reset_info = env.reset(
                            seed=int(np.random.randint(1 << 30))
                        )
                        info = {**info, **reset_info}
                    out.append((observation, reward, terminated, truncated, info))
                remote.send(out)

            elif command == "set":
                name, value = payload
                for env in envs:
                    setattr(env, name, value)
                remote.send(None)

            elif command == "close":
                remote.send(None)
                break
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        remote.close()


class SubprocVecEnv:
    """A fixed set of environments spread across worker processes.

    The interface is deliberately the shape `train.py` already needed: reset
    them all, step them all with one action each, and set an attribute on all
    of them (which is how the curriculum and the shaping weight are applied).
    """

    def __init__(
        self,
        total: int,
        workers: Optional[int] = None,
        env_kwargs: Optional[Dict[str, Any]] = None,
    ):
        # Set first: __del__ runs even if __init__ raises below, and it
        # reads this flag.
        self._closed = False
        self._remotes: List[Any] = []
        self._children: List[Any] = []
        if total < 1:
            raise ValueError("need at least one environment")
        env_kwargs = dict(env_kwargs or {})
        workers = max(1, min(workers or total, total))

        # Spread as evenly as possible; the remainder goes to the first few.
        base, extra = divmod(total, workers)
        self._counts = [base + (1 if i < extra else 0) for i in range(workers)]
        self.total = total
        self.workers = workers

        context = mp.get_context("spawn")
        try:
            for count in self._counts:
                parent, child = context.Pipe()
                process = context.Process(
                    target=_worker,
                    args=(child, parent, count, env_kwargs),
                    daemon=True,
                )
                process.start()
                child.close()
                self._remotes.append(parent)
                self._children.append(process)
        except BaseException:
            # Do not leave half a pool running if one worker fails to start.
            self.close()
            raise

    # ---- the three things training needs ---------------------------------

    def reset(self, seeds: Sequence[int]) -> Tuple[List[Dict], List[Dict]]:
        self._scatter("reset", seeds)
        observations, infos = [], []
        for chunk in self._gather():
            for observation, info in chunk:
                observations.append(observation)
                infos.append(info)
        return observations, infos

    def step(self, actions: Sequence[int]):
        self._scatter("step", actions)
        observations, rewards, dones, infos = [], [], [], []
        for chunk in self._gather():
            for observation, reward, terminated, truncated, info in chunk:
                observations.append(observation)
                rewards.append(float(reward))
                dones.append(bool(terminated or truncated))
                infos.append(info)
        return observations, rewards, dones, infos

    def set_attr(self, name: str, value: Any) -> None:
        for remote in self._remotes:
            remote.send(("set", (name, value)))
        for remote in self._remotes:
            remote.recv()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        for remote in self._remotes:
            try:
                remote.send(("close", None))
                remote.recv()
            except (OSError, EOFError):
                pass
        for process in self._children:
            process.join(timeout=5)
            if process.is_alive():
                process.terminate()

    def __enter__(self) -> "SubprocVecEnv":
        return self

    def __exit__(self, *_) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    # ---- plumbing --------------------------------------------------------

    def _scatter(self, command: str, values: Sequence[Any]) -> None:
        if len(values) != self.total:
            raise ValueError(f"expected {self.total} values, got {len(values)}")
        at = 0
        for remote, count in zip(self._remotes, self._counts):
            remote.send((command, list(values[at : at + count])))
            at += count

    def _gather(self) -> List[Any]:
        return [remote.recv() for remote in self._remotes]


class SequentialVecEnv:
    """The same interface, stepping in this process. Kept as the default.

    Worker processes cost about a second each to start under `spawn`, which is
    not worth paying for a short run or a single environment, and a crash is
    far easier to read without a pool in the way. `train.py` uses this unless
    `--workers` asks for more.
    """

    def __init__(self, total: int, env_kwargs: Optional[Dict[str, Any]] = None):
        from .env import VvWEnv

        self.total = total
        self.workers = 1
        self._envs = [VvWEnv(**dict(env_kwargs or {})) for _ in range(total)]

    def _check(self, values: Sequence[Any]) -> None:
        """Both backends must reject a mismatch identically.

        `zip` would quietly step only as many environments as there were
        actions, which in training corrupts a rollout rather than failing.
        """
        if len(values) != self.total:
            raise ValueError(f"expected {self.total} values, got {len(values)}")

    def reset(self, seeds: Sequence[int]) -> Tuple[List[Dict], List[Dict]]:
        self._check(seeds)
        observations, infos = [], []
        for env, seed in zip(self._envs, seeds):
            observation, info = env.reset(seed=seed)
            observations.append(observation)
            infos.append(info)
        return observations, infos

    def step(self, actions: Sequence[int]):
        self._check(actions)
        observations, rewards, dones, infos = [], [], [], []
        for env, action in zip(self._envs, actions):
            observation, reward, terminated, truncated, info = env.step(action)
            if terminated or truncated:
                # Match the worker pool, which resets on the spot.
                observation, reset_info = env.reset(
                    seed=int(np.random.randint(1 << 30))
                )
                info = {**info, **reset_info}
            observations.append(observation)
            rewards.append(float(reward))
            dones.append(bool(terminated or truncated))
            infos.append(info)
        return observations, rewards, dones, infos

    def set_attr(self, name: str, value: Any) -> None:
        for env in self._envs:
            setattr(env, name, value)

    def close(self) -> None:
        pass

    def __enter__(self) -> "SequentialVecEnv":
        return self

    def __exit__(self, *_) -> None:
        self.close()


def make_vec_env(
    total: int, workers: int = 1, env_kwargs: Optional[Dict[str, Any]] = None
):
    """Pick a pool or a plain loop, depending on how many workers are asked for.

    Measured on twelve cores: 25 environment steps per second in one process,
    against 116 with eight workers and 147 with twelve. The environment is
    pure Python and is the bottleneck, so this is the only setting that turns
    cores into throughput.
    """
    if workers > 1:
        return SubprocVecEnv(total=total, workers=workers, env_kwargs=env_kwargs)
    return SequentialVecEnv(total=total, env_kwargs=env_kwargs)
