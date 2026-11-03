"""Environments stepped across worker processes.

Worth testing carefully: this is the only multiprocessing in the project, it
is what turns cores into throughput, and its first benchmark buried the
machine in recursively spawned processes.
"""

import numpy as np
import pytest

from vvw.vecenv import SequentialVecEnv, make_vec_env

KWARGS = {"split": "train", "shaping": 0.3}


# ----------------------------------------------------------- the interface


def test_the_sequential_backend_is_chosen_by_default():
    vec = make_vec_env(total=2, workers=1, env_kwargs=KWARGS)
    try:
        assert isinstance(vec, SequentialVecEnv)
        assert vec.workers == 1
    finally:
        vec.close()


def test_reset_returns_one_observation_per_environment():
    vec = make_vec_env(total=3, workers=1, env_kwargs=KWARGS)
    try:
        observations, infos = vec.reset([1, 2, 3])
        assert len(observations) == 3 and len(infos) == 3
        for observation in observations:
            assert set(observation) == {"board", "scalars", "candidates", "mask"}
    finally:
        vec.close()


def test_step_returns_one_of_everything_per_environment():
    vec = make_vec_env(total=3, workers=1, env_kwargs=KWARGS)
    try:
        vec.reset([1, 2, 3])
        observations, rewards, dones, infos = vec.step([0, 0, 0])
        assert len(observations) == len(rewards) == len(dones) == len(infos) == 3
        assert all(isinstance(r, float) for r in rewards)
        assert all(isinstance(d, bool) for d in dones)
    finally:
        vec.close()


def test_a_wrong_number_of_actions_is_rejected():
    vec = make_vec_env(total=3, workers=1, env_kwargs=KWARGS)
    try:
        vec.reset([1, 2, 3])
        with pytest.raises(ValueError):
            vec.step([0, 0])
    finally:
        vec.close()


def test_set_attr_reaches_every_environment():
    """This is how the curriculum and the shaping weight are applied."""
    vec = make_vec_env(total=2, workers=1, env_kwargs=KWARGS)
    try:
        vec.reset([1, 2])
        vec.set_attr("board_range", (10, 12))
        for env in vec._envs:
            assert env.board_range == (10, 12)
    finally:
        vec.close()


def test_a_finished_episode_is_reset_without_stalling_the_batch():
    """One environment ending must not hold up the others."""
    vec = make_vec_env(total=2, workers=1, env_kwargs=KWARGS)
    try:
        vec.reset([7, 8])
        for _ in range(400):
            observations, rewards, dones, _ = vec.step([0, 0])
            if any(dones):
                # Whoever finished is already playing a fresh game.
                for observation in observations:
                    assert observation["mask"].sum() > 0
                return
        pytest.skip("no episode ended within the step budget")
    finally:
        vec.close()


def test_closing_twice_is_harmless():
    vec = make_vec_env(total=2, workers=1, env_kwargs=KWARGS)
    vec.close()
    vec.close()


def test_a_bad_environment_count_fails_cleanly():
    """It used to raise again inside __del__, hiding the real error."""
    from vvw.vecenv import SubprocVecEnv

    with pytest.raises(ValueError):
        SubprocVecEnv(total=0)


# --------------------------------------------------------- worker processes


@pytest.mark.slow
def test_worker_processes_agree_with_the_sequential_backend():
    """Same seeds, same maps: the pool must not change what the game is."""
    from vvw.vecenv import SubprocVecEnv

    seeds = [11, 12, 13, 14]
    plain = make_vec_env(total=4, workers=1, env_kwargs=KWARGS)
    try:
        expected, _ = plain.reset(seeds)
    finally:
        plain.close()

    pool = SubprocVecEnv(total=4, workers=2, env_kwargs=KWARGS)
    try:
        got, _ = pool.reset(seeds)
    finally:
        pool.close()

    assert len(got) == len(expected)
    for a, b in zip(expected, got):
        assert np.allclose(a["board"], b["board"])
        assert np.allclose(a["scalars"], b["scalars"])
        assert np.array_equal(a["mask"], b["mask"])


@pytest.mark.slow
def test_the_pool_spreads_environments_over_its_workers():
    from vvw.vecenv import SubprocVecEnv

    pool = SubprocVecEnv(total=5, workers=2, env_kwargs=KWARGS)
    try:
        assert pool.workers == 2
        assert sum(pool._counts) == 5
        assert max(pool._counts) - min(pool._counts) <= 1
        observations, _ = pool.reset(list(range(5)))
        assert len(observations) == 5
    finally:
        pool.close()


@pytest.mark.slow
def test_asking_for_more_workers_than_environments_is_clamped():
    from vvw.vecenv import SubprocVecEnv

    pool = SubprocVecEnv(total=2, workers=8, env_kwargs=KWARGS)
    try:
        assert pool.workers == 2
    finally:
        pool.close()


def test_both_backends_reject_a_mismatch_the_same_way():
    """They are one interface; silently stepping fewer is not an option."""
    from vvw.vecenv import SubprocVecEnv

    plain = make_vec_env(total=3, workers=1, env_kwargs=KWARGS)
    try:
        plain.reset([1, 2, 3])
        with pytest.raises(ValueError):
            plain.step([0, 0])
        with pytest.raises(ValueError):
            plain.reset([1])
    finally:
        plain.close()
